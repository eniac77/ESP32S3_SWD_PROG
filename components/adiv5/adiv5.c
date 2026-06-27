/* ADIv5 transport — DP/AP réteg SWD felett.
 *
 * A swd_phy.h bit-szintű primitíveire épül (családfüggetlen mag, terv 6. szekció):
 *   - JTAG-to-SWD switch + line reset bring-up
 *   - DP/AP regiszter-tranzakciók ACK + paritás kezeléssel
 *   - AHB-AP memória R/W (single + block, TAR auto-increment)
 *
 * Megjegyzés a turnaround-ról: a swd_phy_dir() explicit irányváltást ad
 * (true=meghajt, false=bemenet). Az SWD turnaround periódusban (1 órajel)
 * az irányt váltjuk, majd egy üres órajelet adunk a megfelelő irányban.
 * A swd_phy_seq_in()/seq_out() csak órajelez, az irányt mi állítjuk be.
 */
#include "adiv5.h"
#include "swd_phy.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "adiv5";

/* ---- DP regiszter címek (A[3:2]) ---- */
#define DP_DPIDR     0x0   /* RO  (RnW=1) */
#define DP_ABORT     0x0   /* WO  (RnW=0) */
#define DP_CTRLSTAT  0x4   /* SELECT.DPBANKSEL=0 esetén */
#define DP_SELECT    0x8   /* WO  */
#define DP_RDBUFF    0xC   /* RO  */

/* ---- AHB-AP regiszter offsetek (AP A[3:2] + bank a SELECT.APBANKSEL-ből) ---- */
#define AP_CSW       0x00
#define AP_TAR       0x04
#define AP_DRW       0x0C
#define AP_IDR       0xFC

/* ---- CTRL/STAT bitek ---- */
#define CDBGPWRUPREQ (1u << 28)
#define CDBGPWRUPACK (1u << 29)
#define CSYSPWRUPREQ (1u << 30)
#define CSYSPWRUPACK (1u << 31)

/* ---- ABORT bitek (sticky hibatörlés) ---- */
#define ABORT_DAPABORT   (1u << 0)
#define ABORT_STKCMPCLR  (1u << 1)
#define ABORT_STKERRCLR  (1u << 2)
#define ABORT_WDERRCLR   (1u << 3)
#define ABORT_ORUNERRCLR (1u << 4)
#define ABORT_CLR_ALL    (ABORT_STKCMPCLR | ABORT_STKERRCLR | ABORT_WDERRCLR | ABORT_ORUNERRCLR)

/* ---- AHB-AP CSW mezők ---- */
#define CSW_SIZE_WORD      (2u << 0)  /* 32 bit */
#define CSW_SIZE_MASK      (7u << 0)
#define CSW_ADDRINC_SINGLE (1u << 4)
#define CSW_ADDRINC_MASK   (3u << 4)

/* ---- ACK kódok (3 bit, LSB-first a vezetéken) ---- */
#define ACK_OK    0x1  /* 001 */
#define ACK_WAIT  0x2  /* 010 */
#define ACK_FAULT 0x4  /* 100 */

/* ---- Időzítés / retry konstansok ---- */
#define WAIT_RETRY_MAX   64           /* WAIT ACK újrapróbálkozások */
#define PWRUP_TIMEOUT_US 100000       /* 100 ms a power-up ACK-ra */

/* SELECT árnyékmásolat — felesleges újraírások elkerülésére. */
static uint32_t s_select_shadow;
static bool     s_select_valid;

/* AHB-AP CSW aktuális (visszaolvasott + módosított) értéke. */
static uint32_t s_csw_base;

/* ===================================================================== */
/* Segéd: páros paritás (1-esek számának paritása) egy 32 bites szóra.    */
/* ===================================================================== */
static inline uint32_t parity32(uint32_t v)
{
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1u;
}

/* ===================================================================== */
/* SWD request bájt összeállítása.                                        */
/* Bitkiosztás (LSB-first a vezetéken):                                   */
/*   bit0 Start=1, bit1 APnDP, bit2 RnW, bit3 A[2], bit4 A[3],            */
/*   bit5 paritás(APnDP^RnW^A2^A3), bit6 Stop=0, bit7 Park=1              */
/* Az `addr` az A[3:2] (0x0,0x4,0x8,0xC) — innen A2=bit2, A3=bit3.        */
/* ===================================================================== */
static uint8_t make_request(bool ap, bool rnw, uint32_t addr)
{
    uint32_t apndp = ap ? 1u : 0u;
    uint32_t rd    = rnw ? 1u : 0u;
    uint32_t a2    = (addr >> 2) & 1u;
    uint32_t a3    = (addr >> 3) & 1u;
    uint32_t par   = (apndp ^ rd ^ a2 ^ a3) & 1u;

    uint8_t req = 0;
    req |= 1u << 0;            /* Start = 1 */
    req |= apndp << 1;         /* APnDP   */
    req |= rd << 2;            /* RnW     */
    req |= a2 << 3;            /* A[2]    */
    req |= a3 << 4;            /* A[3]    */
    req |= par << 5;           /* paritás */
    req |= 0u << 6;            /* Stop = 0 */
    req |= 1u << 7;            /* Park = 1 */
    return req;
}

/* ===================================================================== */
/* Alacsony szintű DP/AP olvasás (egy tranzakció).                        */
/* A turnaround-fázisokat itt kezeljük:                                   */
/*   request kiküldése (out) -> TRN (1 clk, váltás bemenetre) ->          */
/*   ACK (3 bit in) -> data (32 bit in) + paritás (1 bit in) ->           */
/*   TRN (1 clk, vissza meghajtásra).                                     */
/* Visszaad: ACK kód (ACK_OK/WAIT/FAULT) vagy 0 protokollhiba esetén.     */
/* ===================================================================== */
static uint32_t swd_read_raw(bool ap, uint32_t addr, uint32_t *data_out)
{
    uint8_t req = make_request(ap, /*rnw=*/true, addr);

    /* request: meghajtott kimenet */
    swd_phy_dir(true);
    swd_phy_seq_out(req, 8);

    /* turnaround: a vonalat a cél hajtja meg (bemenet nekünk) */
    swd_phy_dir(false);
    (void)swd_phy_seq_in(1);                 /* 1 órajel TRN */

    uint32_t ack = swd_phy_seq_in(3) & 0x7u; /* 3 bit ACK */

    if (ack == ACK_OK) {
        uint32_t data = swd_phy_seq_in(32);
        uint32_t par  = swd_phy_seq_in(1) & 1u;

        /* turnaround vissza meghajtásra */
        swd_phy_dir(true);
        swd_phy_seq_out(0, 1);               /* 1 órajel TRN */

        if (parity32(data) != par) {
            ESP_LOGW(TAG, "olvasás paritáshiba (%s addr=0x%lx, adat=0x%08lx, vart par=%lu, kapott par=%lu)",
                     ap ? "AP" : "DP", (unsigned long)addr, (unsigned long)data,
                     (unsigned long)parity32(data), (unsigned long)par);
            return 0;                        /* paritáshiba -> protokollhiba */
        }
        if (data_out) *data_out = data;
        ESP_LOGV(TAG, "raw RD  %s addr=0x%lx ACK=OK adat=0x%08lx",
                 ap ? "AP" : "DP", (unsigned long)addr, (unsigned long)data);
        return ACK_OK;
    }

    /* WAIT/FAULT: nincs data fázis, de a turnaround vissza kell. */
    swd_phy_dir(true);
    swd_phy_seq_out(0, 1);                    /* 1 órajel TRN */
    ESP_LOGV(TAG, "raw RD  %s addr=0x%lx ACK=%s (0x%lx)",
             ap ? "AP" : "DP", (unsigned long)addr,
             ack == ACK_WAIT ? "WAIT" : (ack == ACK_FAULT ? "FAULT" : "???"),
             (unsigned long)ack);
    return ack;
}

/* ===================================================================== */
/* Alacsony szintű DP/AP írás (egy tranzakció).                           */
/*   request (out) -> TRN (in) -> ACK (3 bit in) ->                       */
/*   TRN (vissza out) -> data (32 bit out) + paritás (1 bit out).         */
/* Visszaad: ACK kód.                                                     */
/* ===================================================================== */
static uint32_t swd_write_raw(bool ap, uint32_t addr, uint32_t data)
{
    uint8_t req = make_request(ap, /*rnw=*/false, addr);

    swd_phy_dir(true);
    swd_phy_seq_out(req, 8);

    /* turnaround bemenetre az ACK-hoz */
    swd_phy_dir(false);
    (void)swd_phy_seq_in(1);                 /* 1 órajel TRN */

    uint32_t ack = swd_phy_seq_in(3) & 0x7u;

    /* turnaround vissza meghajtásra (íráshoz mindenképp) */
    swd_phy_dir(true);
    swd_phy_seq_out(0, 1);                    /* 1 órajel TRN */

    if (ack == ACK_OK) {
        swd_phy_seq_out(data, 32);
        swd_phy_seq_out(parity32(data), 1);
        ESP_LOGV(TAG, "raw WR  %s addr=0x%lx ACK=OK adat=0x%08lx",
                 ap ? "AP" : "DP", (unsigned long)addr, (unsigned long)data);
    } else {
        ESP_LOGV(TAG, "raw WR  %s addr=0x%lx ACK=%s (0x%lx) adat=0x%08lx",
                 ap ? "AP" : "DP", (unsigned long)addr,
                 ack == ACK_WAIT ? "WAIT" : (ack == ACK_FAULT ? "FAULT" : "???"),
                 (unsigned long)ack, (unsigned long)data);
    }
    return ack;
}

/* ===================================================================== */
/* Sticky hiba törlése ABORT regiszteren keresztül (FAULT után).          */
/* Az ABORT mindig elérhető, nem függ a SELECT-től.                       */
/* ===================================================================== */
static void dp_abort_clear(void)
{
    ESP_LOGD(TAG, "ABORT: sticky hibák törlése (ABORT<-0x%02x)", ABORT_CLR_ALL);
    (void)swd_write_raw(/*ap=*/false, DP_ABORT, ABORT_CLR_ALL);
}

/* ===================================================================== */
/* DP olvasás/írás WAIT-retry + FAULT->ABORT kezeléssel.                  */
/* ===================================================================== */
static esp_err_t dp_read(uint32_t addr, uint32_t *val)
{
    for (int i = 0; i < WAIT_RETRY_MAX; i++) {
        uint32_t ack = swd_read_raw(false, addr, val);
        if (ack == ACK_OK) {
            ESP_LOGV(TAG, "DP RD  addr=0x%lx -> 0x%08lx%s",
                     (unsigned long)addr, (unsigned long)(val ? *val : 0),
                     i ? " (WAIT-retry után)" : "");
            return ESP_OK;
        }
        if (ack == ACK_WAIT) {
            ESP_LOGV(TAG, "DP RD  addr=0x%lx WAIT retry #%d/%d",
                     (unsigned long)addr, i + 1, WAIT_RETRY_MAX);
            continue;                /* újrapróba */
        }
        if (ack == ACK_FAULT) {
            ESP_LOGW(TAG, "DP RD  addr=0x%lx FAULT -> ABORT", (unsigned long)addr);
            dp_abort_clear();
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "DP RD  addr=0x%lx ismeretlen/protokollhiba (ACK=0x%lx)",
                 (unsigned long)addr, (unsigned long)ack);
        return ESP_ERR_INVALID_RESPONSE;              /* 0/ismeretlen ACK */
    }
    ESP_LOGW(TAG, "DP olvasás: tartós WAIT (addr=0x%lx, %d retry kimerült)",
             (unsigned long)addr, WAIT_RETRY_MAX);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t dp_write(uint32_t addr, uint32_t val)
{
    for (int i = 0; i < WAIT_RETRY_MAX; i++) {
        uint32_t ack = swd_write_raw(false, addr, val);
        if (ack == ACK_OK) {
            ESP_LOGV(TAG, "DP WR  addr=0x%lx <- 0x%08lx%s",
                     (unsigned long)addr, (unsigned long)val,
                     i ? " (WAIT-retry után)" : "");
            return ESP_OK;
        }
        if (ack == ACK_WAIT) {
            ESP_LOGV(TAG, "DP WR  addr=0x%lx WAIT retry #%d/%d",
                     (unsigned long)addr, i + 1, WAIT_RETRY_MAX);
            continue;
        }
        if (ack == ACK_FAULT) {
            ESP_LOGW(TAG, "DP WR  addr=0x%lx FAULT -> ABORT", (unsigned long)addr);
            dp_abort_clear();
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "DP WR  addr=0x%lx ismeretlen/protokollhiba (ACK=0x%lx)",
                 (unsigned long)addr, (unsigned long)ack);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGW(TAG, "DP írás: tartós WAIT (addr=0x%lx, %d retry kimerült)",
             (unsigned long)addr, WAIT_RETRY_MAX);
    return ESP_ERR_TIMEOUT;
}

/* ===================================================================== */
/* SELECT beállítása: AP-választás (mindig AP0) + bankok.                 */
/* apbanksel = AP regiszter A[7:4] nibble; dpbanksel = DP bank A[3:0].    */
/* ===================================================================== */
static esp_err_t dp_select(uint32_t apbanksel, uint32_t dpbanksel)
{
    /* SELECT: [31:24]=APSEL(0), [7:4]=APBANKSEL, [3:0]=DPBANKSEL */
    uint32_t sel = ((apbanksel & 0xFu) << 4) | (dpbanksel & 0xFu);
    if (s_select_valid && sel == s_select_shadow) return ESP_OK;

    ESP_LOGV(TAG, "SELECT írás 0x%08lx (APBANKSEL=0x%lx, DPBANKSEL=0x%lx)",
             (unsigned long)sel, (unsigned long)(apbanksel & 0xFu),
             (unsigned long)(dpbanksel & 0xFu));
    esp_err_t err = dp_write(DP_SELECT, sel);
    if (err == ESP_OK) {
        s_select_shadow = sel;
        s_select_valid  = true;
    } else {
        s_select_valid = false;
    }
    return err;
}

/* ===================================================================== */
/* AHB-AP alacsony szintű hozzáférések.                                   */
/* `ap_off` az AP regiszter teljes 8 bites offsetje (pl. 0xFC=IDR);       */
/* a felső nibble a SELECT.APBANKSEL, az alsó (A[3:2]) a request címe.    */
/*                                                                        */
/* FONTOS (posted read): egy AP-olvasás kérése nem a kért regiszter       */
/* aktuális értékét adja vissza, hanem az ELŐZŐ AP-olvasásét. A friss     */
/* érték a következő AP-olvasásból vagy a DP RDBUFF-ból jön ki.           */
/* ===================================================================== */
static esp_err_t ap_read_posted(uint32_t ap_off, uint32_t *prev_val)
{
    esp_err_t err = dp_select((ap_off >> 4) & 0xFu, 0);
    if (err != ESP_OK) return err;

    for (int i = 0; i < WAIT_RETRY_MAX; i++) {
        uint32_t ack = swd_read_raw(/*ap=*/true, ap_off & 0xCu, prev_val);
        if (ack == ACK_OK) {
            ESP_LOGV(TAG, "AP RD (posted) off=0x%02lx -> prev=0x%08lx%s",
                     (unsigned long)ap_off, (unsigned long)(prev_val ? *prev_val : 0),
                     i ? " (WAIT-retry után)" : "");
            return ESP_OK;
        }
        if (ack == ACK_WAIT) {
            ESP_LOGV(TAG, "AP RD off=0x%02lx WAIT retry #%d/%d",
                     (unsigned long)ap_off, i + 1, WAIT_RETRY_MAX);
            continue;
        }
        if (ack == ACK_FAULT) {
            ESP_LOGW(TAG, "AP RD off=0x%02lx FAULT -> ABORT", (unsigned long)ap_off);
            dp_abort_clear();
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "AP RD off=0x%02lx ismeretlen/protokollhiba (ACK=0x%lx)",
                 (unsigned long)ap_off, (unsigned long)ack);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGW(TAG, "AP olvasás: tartós WAIT (off=0x%02lx)", (unsigned long)ap_off);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t ap_write(uint32_t ap_off, uint32_t val)
{
    esp_err_t err = dp_select((ap_off >> 4) & 0xFu, 0);
    if (err != ESP_OK) return err;

    for (int i = 0; i < WAIT_RETRY_MAX; i++) {
        uint32_t ack = swd_write_raw(/*ap=*/true, ap_off & 0xCu, val);
        if (ack == ACK_OK) {
            ESP_LOGV(TAG, "AP WR off=0x%02lx <- 0x%08lx%s",
                     (unsigned long)ap_off, (unsigned long)val,
                     i ? " (WAIT-retry után)" : "");
            return ESP_OK;
        }
        if (ack == ACK_WAIT) {
            ESP_LOGV(TAG, "AP WR off=0x%02lx WAIT retry #%d/%d",
                     (unsigned long)ap_off, i + 1, WAIT_RETRY_MAX);
            continue;
        }
        if (ack == ACK_FAULT) {
            ESP_LOGW(TAG, "AP WR off=0x%02lx FAULT -> ABORT", (unsigned long)ap_off);
            dp_abort_clear();
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "AP WR off=0x%02lx ismeretlen/protokollhiba (ACK=0x%lx)",
                 (unsigned long)ap_off, (unsigned long)ack);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGW(TAG, "AP írás: tartós WAIT (off=0x%02lx)", (unsigned long)ap_off);
    return ESP_ERR_TIMEOUT;
}

/* AP olvasás "kényelmi" formája: kiad egy AP-olvasást (indítja a hozzá-
   férést), majd a tényleges adatot a DP RDBUFF-ból szedi ki (posted read).
   Egyetlen logikai értéket ad vissza. */
static esp_err_t ap_read(uint32_t ap_off, uint32_t *val)
{
    uint32_t dummy;
    esp_err_t err = ap_read_posted(ap_off, &dummy);
    if (err != ESP_OK) return err;
    return dp_read(DP_RDBUFF, val);
}

/* ===================================================================== */
/* AHB-AP CSW alapállapot: reset-default bitek megtartása RMW-vel,        */
/* Size=Word + AddrInc=Single beállítása.                                 */
/* ===================================================================== */
static esp_err_t ahb_csw_init(void)
{
    uint32_t csw = 0;
    esp_err_t err = ap_read(AP_CSW, &csw);
    if (err != ESP_OK) return err;

    /* Méret és inkremens mezők törlése, majd beállítás. A többi (Prot,
       SPIDEN, DbgSwEnable, MasterType stb.) reset-default bitet megtartjuk. */
    ESP_LOGV(TAG, "AHB-AP CSW kiindulás (reset-default) = 0x%08lx", (unsigned long)csw);
    csw &= ~(CSW_SIZE_MASK | CSW_ADDRINC_MASK);
    csw |= CSW_SIZE_WORD | CSW_ADDRINC_SINGLE;

    err = ap_write(AP_CSW, csw);
    if (err != ESP_OK) return err;

    s_csw_base = csw;
    ESP_LOGD(TAG, "AHB-AP CSW beállítva (Word + AddrInc Single) = 0x%08lx",
             (unsigned long)csw);
    return ESP_OK;
}

/* Block hozzáférés elején a CSW-t (AddrInc=Single) újraírjuk, hogy a
   TAR-növelés garantáltan a Single módban induljon. */
static esp_err_t ahb_set_addrinc_single(void)
{
    uint32_t csw = (s_csw_base & ~(CSW_ADDRINC_MASK | CSW_SIZE_MASK))
                 | CSW_ADDRINC_SINGLE | CSW_SIZE_WORD;
    s_csw_base = csw;
    return ap_write(AP_CSW, csw);
}

/* ===================================================================== */
/* Publikus API                                                          */
/* ===================================================================== */

esp_err_t adiv5_init(void)
{
    s_select_valid = false;
    s_csw_base = 0;

    esp_err_t err = swd_phy_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "swd_phy_init hiba: %s", esp_err_to_name(err));
        return err;
    }
    /* Bring-up alatt lassú órajel ajánlott (terv 5. szekció). */
    swd_phy_set_freq_hz(300000);
    ESP_LOGI(TAG, "adiv5_init OK, SWD órajel = 300 kHz (bring-up)");
    return ESP_OK;
}

/* Line reset: legalább 50 órajel SWDIO=1-en tartva. */
static void line_reset(void)
{
    ESP_LOGD(TAG, "line reset (>=50 SWCLK, SWDIO=1)");
    swd_phy_dir(true);
    swd_phy_idle(56);   /* >= 50 órajel SWDIO=1 */
}

esp_err_t adiv5_connect(uint32_t *idcode_out)
{
    s_select_valid = false;
    s_csw_base = 0;

    ESP_LOGD(TAG, "SWD bring-up indul");

    /* 1) Line reset */
    line_reset();

    /* 2) JTAG-to-SWD switch: 0xE79E, LSB-first 16 bit.
       A vezetéken így 0x9E, 0xE7 jelenik meg (a seq_out LSB-first küld). */
    ESP_LOGD(TAG, "JTAG->SWD switch küldése (0xE79E, 16 bit LSB-first)");
    swd_phy_dir(true);
    swd_phy_seq_out(0xE79E, 16);

    /* 3) Újabb line reset */
    line_reset();

    /* Néhány idle bit a switch után, az első tranzakció előtt. */
    ESP_LOGV(TAG, "8 idle bit a switch után");
    swd_phy_dir(true);
    swd_phy_seq_out(0, 8);

    /* 4) DPIDR olvasás (DP 0x0) — első érvényes tranzakció. */
    ESP_LOGD(TAG, "DPIDR olvasás (első érvényes tranzakció)");
    uint32_t dpidr = 0;
    esp_err_t err = dp_read(DP_DPIDR, &dpidr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DPIDR olvasás sikertelen: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "DPIDR = 0x%08lx", (unsigned long)dpidr);
    if (idcode_out) *idcode_out = dpidr;

    /* 5) SELECT nullázás (AP0, bank0) — ismert kiindulás. */
    ESP_LOGD(TAG, "SELECT nullázás (AP0, bank0)");
    err = dp_select(0, 0);
    if (err != ESP_OK) return err;

    /* 6) Esetleges sticky hibák törlése a tiszta indulásért. */
    dp_abort_clear();

    /* 7) Debug + rendszer power-up kérése. */
    ESP_LOGD(TAG, "CTRL/STAT power-up kérés (CDBGPWRUPREQ | CSYSPWRUPREQ)");
    err = dp_write(DP_CTRLSTAT, CDBGPWRUPREQ | CSYSPWRUPREQ);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CTRL/STAT power-up írás hiba: %s", esp_err_to_name(err));
        return err;
    }

    /* 8) Várakozás az ACK bitekre (CDBGPWRUPACK + CSYSPWRUPACK). */
    ESP_LOGD(TAG, "várakozás CDBGPWRUPACK + CSYSPWRUPACK bitekre");
    int64_t t0 = esp_timer_get_time();
    uint32_t stat = 0;
    for (;;) {
        err = dp_read(DP_CTRLSTAT, &stat);
        if (err != ESP_OK) return err;
        if ((stat & (CDBGPWRUPACK | CSYSPWRUPACK)) == (CDBGPWRUPACK | CSYSPWRUPACK)) {
            break;
        }
        if (esp_timer_get_time() - t0 > PWRUP_TIMEOUT_US) {
            ESP_LOGE(TAG, "power-up ACK timeout (CTRL/STAT=0x%08lx)", (unsigned long)stat);
            return ESP_ERR_TIMEOUT;
        }
    }
    ESP_LOGI(TAG, "debug power-up OK (CTRL/STAT=0x%08lx, CDBGPWRUPACK+CSYSPWRUPACK megérkezett, %lld us)",
             (unsigned long)stat, (long long)(esp_timer_get_time() - t0));

    /* 9) AHB-AP CSW alapállapot (Word + AddrInc Single, RMW). */
    ESP_LOGD(TAG, "AHB-AP CSW alapállapot beállítása");
    err = ahb_csw_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AHB-AP CSW init hiba: %s", esp_err_to_name(err));
        return err;
    }

    /* Diagnosztika: AP IDR (nem kritikus, csak log). */
    uint32_t idr = 0;
    if (ap_read(AP_IDR, &idr) == ESP_OK) {
        ESP_LOGI(TAG, "AHB-AP IDR = 0x%08lx", (unsigned long)idr);
    }

    ESP_LOGI(TAG, "SWD bring-up KÉSZ (DPIDR=0x%08lx)", (unsigned long)dpidr);
    return ESP_OK;
}

esp_err_t adiv5_read32(uint32_t addr, uint32_t *val)
{
    if (!val) return ESP_ERR_INVALID_ARG;

    esp_err_t err = ap_write(AP_TAR, addr);
    if (err != ESP_OK) return err;

    /* DRW olvasás posted -> a tényleges adat RDBUFF-ból (ap_read kezeli). */
    err = ap_read(AP_DRW, val);
    if (err == ESP_OK) {
        ESP_LOGV(TAG, "rd32 0x%08lx -> 0x%08lx", (unsigned long)addr, (unsigned long)*val);
    } else {
        ESP_LOGW(TAG, "rd32 0x%08lx hiba: %s", (unsigned long)addr, esp_err_to_name(err));
    }
    return err;
}

esp_err_t adiv5_write32(uint32_t addr, uint32_t val)
{
    esp_err_t err = ap_write(AP_TAR, addr);
    if (err != ESP_OK) return err;

    err = ap_write(AP_DRW, val);
    if (err == ESP_OK) {
        ESP_LOGV(TAG, "wr32 0x%08lx <- 0x%08lx", (unsigned long)addr, (unsigned long)val);
    } else {
        ESP_LOGW(TAG, "wr32 0x%08lx <- 0x%08lx hiba: %s",
                 (unsigned long)addr, (unsigned long)val, esp_err_to_name(err));
    }
    return err;
}

/* ===================================================================== */
/* Block olvasás. A TAR a Single AddrInc miatt automatikusan nő minden    */
/* DRW hozzáférésnél, de csak 1 KB-os (10 bites) határon belül; a határt   */
/* átlépve a TAR-t újra kell írni. Posted-read miatt egy plusz indító      */
/* olvasás kell, és az utolsó valódi adatot RDBUFF-ból olvassuk.           */
/* ===================================================================== */
esp_err_t adiv5_read_block(uint32_t addr, uint32_t *buf, size_t words)
{
    if (!buf) return ESP_ERR_INVALID_ARG;
    if (words == 0) return ESP_OK;

    ESP_LOGD(TAG, "block RD  addr=0x%08lx, %u szó (%u bájt)",
             (unsigned long)addr, (unsigned)words, (unsigned)(words * 4));

    esp_err_t err = ahb_set_addrinc_single();
    if (err != ESP_OK) return err;

    size_t i = 0;
    while (i < words) {
        /* Hátralévő szavak a jelenlegi 1 KB-os blokkban (TAR auto-inc határa). */
        uint32_t off_in_kb = addr & 0x3FFu;                 /* 0..1023 bájt */
        uint32_t words_left_in_kb = (1024u - off_in_kb) / 4u;
        size_t chunk = words - i;
        if (chunk > words_left_in_kb) chunk = words_left_in_kb;

        /* TAR a blokk elejére. */
        err = ap_write(AP_TAR, addr);
        if (err != ESP_OK) return err;

        /* Posted-read indítás: az első DRW olvasás eredménye eldobandó. */
        uint32_t dummy;
        err = ap_read_posted(AP_DRW, &dummy);
        if (err != ESP_OK) return err;

        /* A chunk első (chunk-1) szava újabb DRW olvasásokból (mindegyik az
           ELŐZŐ hozzáférés adatát adja vissza), az utolsó RDBUFF-ból. */
        for (size_t k = 1; k < chunk; k++) {
            err = ap_read_posted(AP_DRW, &buf[i + k - 1]);
            if (err != ESP_OK) return err;
        }
        /* Az utolsó valódi adat RDBUFF-ból (ez nem növeli a TAR-t). */
        err = dp_read(DP_RDBUFF, &buf[i + chunk - 1]);
        if (err != ESP_OK) return err;

        i    += chunk;
        addr += (uint32_t)chunk * 4u;
    }
    return ESP_OK;
}

/* ===================================================================== */
/* Block írás. TAR auto-inc Single; 1 KB határon TAR újraírás. Íráskor    */
/* nincs posted-read bonyodalom.                                          */
/* ===================================================================== */
esp_err_t adiv5_write_block(uint32_t addr, const uint32_t *buf, size_t words)
{
    if (!buf) return ESP_ERR_INVALID_ARG;
    if (words == 0) return ESP_OK;

    ESP_LOGD(TAG, "block WR  addr=0x%08lx, %u szó (%u bájt)",
             (unsigned long)addr, (unsigned)words, (unsigned)(words * 4));

    esp_err_t err = ahb_set_addrinc_single();
    if (err != ESP_OK) return err;

    size_t i = 0;
    while (i < words) {
        uint32_t off_in_kb = addr & 0x3FFu;
        uint32_t words_left_in_kb = (1024u - off_in_kb) / 4u;
        size_t chunk = words - i;
        if (chunk > words_left_in_kb) chunk = words_left_in_kb;

        err = ap_write(AP_TAR, addr);
        if (err != ESP_OK) return err;

        for (size_t k = 0; k < chunk; k++) {
            err = ap_write(AP_DRW, buf[i + k]);
            if (err != ESP_OK) return err;
        }

        i    += chunk;
        addr += (uint32_t)chunk * 4u;
    }
    return ESP_OK;
}
