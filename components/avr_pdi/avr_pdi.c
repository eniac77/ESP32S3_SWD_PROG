/* AVR PDI programozó (XMEGA) — bit-bang PHY + PDI link + XMEGA NVM-folyam.
 *
 * Réteg-felépítés (alulról):
 *   PHY     : bit-bang PDI_CLK (kimenet) + PDI_DATA (kétirányú, turnaround).
 *             12-bites keret: start(0) + 8 adat LSB-first + páros paritás + 2 stop(1).
 *             A PDI-nak NINCS SYNC-bájtja (ellentétben az UPDI-val) és NINCS ACK-ja.
 *   Link    : enable + PDI utasítások (LDS/STS/LD/ST/LDCS/STCS/KEY), idle/guard.
 *   NVM     : XMEGA NVM-vezérlő (ERASE_FLASH_BUFFER/CMDEX, LOAD_FLASH_BUFFER,
 *             ERASE_WRITE_FLASH_PAGE flash-címre írással triggerelve, READ_NVM).
 *   Orchestr: enable -> NVM key -> per-lap erase+write -> verify -> reset.
 *
 * A konstansok az avrdude + a pdi-pruss XMEGA NVM-driver + avr-libc iox*.h
 * forrásokhoz igazítva. FONTOS eltérés a korábbi vázhoz: az I/O-regiszterek a
 * PDI data-space bázisán vannak (0x01000000+), tehát az NVM-vezérlő valós címe
 * 0x010001C0, a MCU.DEVID 0x01000090 — ezért 4-bájtos (long) címzés kell.
 * A flash a PDI cím-térben 0x00800000-nál van.
 *
 * HW-n MÉG NEM IGAZOLT. Lásd: reference/AVR_PDI.md.
 */
#include "avr_pdi.h"

#include <string.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage_src.h"
#include "avr_image.h"

static const char *TAG = "avr_pdi";

/* ============================ Lábkiosztás (Kconfig) ===================== */
#define PIN_CLK   CONFIG_AVR_PDI_CLK_GPIO    /* PDI_CLK = target RESET (default 21) */
#define PIN_DATA  CONFIG_AVR_PDI_DATA_GPIO   /* PDI_DATA = MISO (default 7) */

/* Fél-órajel: ~3 us -> ~150 kHz PDI clock. Szinkron → clock-stretch nélkül is jó. */
#define PDI_HALF_US   3

static bool s_inited = false;

/* ============================ PDI link konstansok ====================== */
/* Base-opkódok. */
#define OP_LDS    0x00
#define OP_STS    0x40
#define OP_LD     0x20
#define OP_ST     0x60
#define OP_LDCS   0x80
#define OP_STCS   0xC0
#define OP_REPEAT 0xA0
#define OP_KEY    0xE0
/* cím-méret bitek (LDS/STS bit3:2): */
#define ADDR_1B   0x00
#define ADDR_2B   0x04
#define ADDR_3B   0x08
#define ADDR_4B   0x0C
/* adat-méret bitek (bit1:0): */
#define DATA_1B   0x00
#define DATA_2B   0x01
/* pointer-módok (LD/ST bit3:2): */
#define PTR_STAR      0x00
#define PTR_STAR_INC  0x04
#define PTR_ADDRESS   0x08

/* Származtatott opkódok — 4-bájtos (long) címzés (a 0x010001xx I/O-címekhez kell). */
#define LDS_L     (OP_LDS | ADDR_4B | DATA_1B)   /* 0x0C */
#define STS_L     (OP_STS | ADDR_4B | DATA_1B)   /* 0x4C */
#define ST_INC_B  (OP_ST  | PTR_STAR_INC | DATA_1B) /* 0x64: *(ptr++) bájt */

/* ST-pointer beállítás long (4-bájtos) címmel: ST | PTR_ADDRESS | data-size(long).
 * A data-size mező kódolja a cím bájtszámát a pointer-setnél; long = 0x03. */
#define ST_PTR_LONG  (OP_ST | PTR_ADDRESS | 0x03) /* 0x6B: ptr set, 4 cím-bájt */

/* PDI control/status regiszterek (LDCS/STCS index). */
#define PDI_REG_STATUS  0x00     /* NVMEN = bit1 */
#define PDI_REG_RESET   0x01     /* 0x59 = reset apply, 0x00 = release */
#define PDI_REG_CTRL    0x02     /* guard time (GT) bits[2:0] */

#define PDI_STATUS_NVMEN  0x02
#define PDI_RESET_KEY     0x59
#define PDI_GT_CONSV      0x02   /* guard time +32 idle bit (óvatos, bring-up-hoz) */

/* NVM KEY (64-bit 0x1289AB45CDD888FF) — a vonalra LSB-first. */
static const uint8_t PDI_NVM_KEY[8] = {
    0xFF, 0x88, 0xD8, 0xCD, 0x45, 0xAB, 0x89, 0x12
};

/* ============================ XMEGA NVM-vezérlő ======================== */
/* Az I/O-regiszterek a PDI data-space bázisán (0x01000000) vannak. */
#define PDI_DATA_BASE   0x01000000UL
#define NVM_BASE        (PDI_DATA_BASE + 0x01C0)  /* 0x010001C0 */
#define NVM_CMD         (NVM_BASE + 0x0A)         /* 0x010001CA */
#define NVM_CTRLA       (NVM_BASE + 0x0B)         /* 0x010001CB, CMDEX = bit0 */
#define NVM_STATUS      (NVM_BASE + 0x0F)         /* 0x010001CF */

#define NVM_CMDEX        0x01
#define NVM_STATUS_BUSY  0x80   /* NVMBUSY (bit7) */
#define NVM_STATUS_FBUSY 0x40   /* FBUSY (bit6) */

/* NVM parancsok (CMD). */
#define NVMCMD_NO_OP                0x00
#define NVMCMD_CHIP_ERASE           0x40
#define NVMCMD_READ_NVM             0x43
#define NVMCMD_LOAD_FLASH_BUFFER    0x23
#define NVMCMD_ERASE_FLASH_BUFFER   0x26
#define NVMCMD_ERASE_WRITE_FLASH    0x2F   /* erase+write flash page */

/* MCU.DEVID (signature) az I/O-térben -> data-space cím. */
#define MCU_DEVID0      (PDI_DATA_BASE + 0x0090)  /* 0x01000090 */

/* Flash leképzése a PDI cím-térben (XMEGA app flash, saját bázis — NEM I/O). */
#define PDI_FLASH_BASE  0x00800000UL

/* ============================ Cél-tábla ================================ */
typedef struct {
    uint8_t  sig[3];
    const char *name;
    uint32_t flash_size;   /* app+boot flash, bájt (avrdude.conf) */
    uint16_t page_size;    /* bájt */
} pdi_known_t;

/* avrdude.conf + A4U datasheet szerint (page-size). */
static const pdi_known_t PDI_TABLE[] = {
    { { 0x1E, 0x95, 0x41 }, "ATxmega32A4(U)", 0x9000,  256 },
    { { 0x1E, 0x96, 0x42 }, "ATxmega64A3",    0x22000, 512 },
    { { 0x1E, 0x97, 0x4C }, "ATxmega128A1",   0x22000, 512 },
    { { 0x1E, 0x97, 0x42 }, "ATxmega128A3",   0x22000, 512 },
    { { 0x1E, 0x98, 0x42 }, "ATxmega256A3",   0x42000, 512 },
};

static const pdi_known_t *pdi_lookup(const uint8_t sig[3])
{
    for (size_t i = 0; i < sizeof(PDI_TABLE) / sizeof(PDI_TABLE[0]); ++i) {
        if (memcmp(PDI_TABLE[i].sig, sig, 3) == 0) return &PDI_TABLE[i];
    }
    return NULL;
}

/* ============================ PHY (bit-bang) =========================== */

static inline void clk_high(void) { gpio_set_level(PIN_CLK, 1); esp_rom_delay_us(PDI_HALF_US); }
static inline void clk_low(void)  { gpio_set_level(PIN_CLK, 0); esp_rom_delay_us(PDI_HALF_US); }

/* PDI_DATA irány: drive=true -> kimenet (mi hajtjuk), false -> bemenet (cél hajtja). */
static void data_dir(bool drive)
{
    gpio_set_direction(PIN_DATA, drive ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
}

/* Egy bit kiküldése: adat beáll, felfutó él (cél mintázza), lefutó él. */
static void tx_bit(int bit)
{
    gpio_set_level(PIN_DATA, bit & 1);
    esp_rom_delay_us(PDI_HALF_US);
    clk_high();
    clk_low();
}

/* Egy bit beolvasása: felfutó él, minta a magas szakaszon, lefutó él. */
static int rx_bit(void)
{
    gpio_set_level(PIN_CLK, 1);
    esp_rom_delay_us(PDI_HALF_US);
    int b = gpio_get_level(PIN_DATA);
    gpio_set_level(PIN_CLK, 0);
    esp_rom_delay_us(PDI_HALF_US);
    return b;
}

/* Idle bitek (DATA = 1). */
static void tx_idle(int bits)
{
    gpio_set_level(PIN_DATA, 1);
    for (int i = 0; i < bits; ++i) { clk_high(); clk_low(); }
}

/* 12-bites keret küldése: start(0) + 8 adat LSB-first + páros paritás + 2 stop(1). */
static void pdi_tx_byte(uint8_t b)
{
    int par = 0;
    tx_bit(0);
    for (int i = 0; i < 8; ++i) {
        int bit = (b >> i) & 1;
        par ^= bit;
        tx_bit(bit);
    }
    tx_bit(par);
    tx_bit(1); tx_bit(1);
}

/* 12-bites keret fogadása: start-bit keresése (max idle), 8 adat, paritás, 2 stop. */
static esp_err_t pdi_rx_byte(uint8_t *out)
{
    int found = 0;
    for (int n = 0; n < 64; ++n) {
        if (rx_bit() == 0) { found = 1; break; }
    }
    if (!found) { ESP_LOGW(TAG, "RX: nincs start-bit (timeout)"); return ESP_ERR_TIMEOUT; }

    int par = 0; uint8_t v = 0;
    for (int i = 0; i < 8; ++i) {
        int bit = rx_bit();
        par ^= bit;
        v |= (bit << i);
    }
    int pbit = rx_bit();
    int s1 = rx_bit(); (void)rx_bit();
    if (pbit != par) { ESP_LOGW(TAG, "RX: paritashiba (%02X)", v); return ESP_FAIL; }
    if (s1 != 1)     { ESP_LOGW(TAG, "RX: framing (stop=%d)", s1); return ESP_FAIL; }
    *out = v;
    return ESP_OK;
}

/* ============================ Link-réteg ============================== */

static void pdi_send(const uint8_t *buf, size_t n)
{
    data_dir(true);
    for (size_t i = 0; i < n; ++i) pdi_tx_byte(buf[i]);
}

static esp_err_t pdi_recv(uint8_t *buf, size_t n)
{
    data_dir(false);
    for (size_t i = 0; i < n; ++i) {
        esp_err_t err = pdi_rx_byte(&buf[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static void pdi_stcs(uint8_t reg, uint8_t val)
{
    uint8_t f[2] = { (uint8_t)(OP_STCS | (reg & 0x0F)), val };
    pdi_send(f, sizeof(f));
}

static esp_err_t pdi_ldcs(uint8_t reg, uint8_t *out)
{
    uint8_t f[1] = { (uint8_t)(OP_LDCS | (reg & 0x0F)) };
    pdi_send(f, sizeof(f));
    return pdi_recv(out, 1);
}

/* STS (4-bájtos cím, 1 adatbájt) — PDI-nak nincs ACK-ja. */
static void pdi_sts8(uint32_t addr, uint8_t data)
{
    uint8_t f[6] = { STS_L,
                     (uint8_t)addr, (uint8_t)(addr >> 8),
                     (uint8_t)(addr >> 16), (uint8_t)(addr >> 24),
                     data };
    pdi_send(f, sizeof(f));
}

/* LDS (4-bájtos cím, 1 adatbájt). */
static esp_err_t pdi_lds8(uint32_t addr, uint8_t *out)
{
    uint8_t f[5] = { LDS_L,
                     (uint8_t)addr, (uint8_t)(addr >> 8),
                     (uint8_t)(addr >> 16), (uint8_t)(addr >> 24) };
    pdi_send(f, sizeof(f));
    return pdi_recv(out, 1);
}

/* ST-pointer beállítása (4-bájtos cím). */
static void pdi_st_ptr(uint32_t addr)
{
    uint8_t f[5] = { ST_PTR_LONG,
                     (uint8_t)addr, (uint8_t)(addr >> 8),
                     (uint8_t)(addr >> 16), (uint8_t)(addr >> 24) };
    pdi_send(f, sizeof(f));
}

/* ST *(ptr++) = bájt. */
static void pdi_st_inc_byte(uint8_t data)
{
    uint8_t f[2] = { ST_INC_B, data };
    pdi_send(f, sizeof(f));
}

/* NVM STATUS BUSY poll. */
static esp_err_t pdi_nvm_wait(int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    do {
        uint8_t st;
        if (pdi_lds8(NVM_STATUS, &st) == ESP_OK) {
            if ((st & (NVM_STATUS_BUSY | NVM_STATUS_FBUSY)) == 0) return ESP_OK;
        }
        esp_rom_delay_us(200);
    } while (esp_timer_get_time() < deadline);
    ESP_LOGE(TAG, "NVM BUSY timeout");
    return ESP_ERR_TIMEOUT;
}

/* NVM parancs + CMDEX trigger (a "manuális" parancsokhoz: buffer/chip erase). */
static void pdi_nvm_cmd(uint8_t cmd) { pdi_sts8(NVM_CMD, cmd); }
static void pdi_nvm_exec(void)       { pdi_sts8(NVM_CTRLA, NVM_CMDEX); }

/* ============================ GPIO init =============================== */
esp_err_t avr_pdi_init(void)
{
    if (s_inited) return ESP_OK;
    gpio_config_t clk = {
        .pin_bit_mask = (1ULL << PIN_CLK),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&clk);
    if (err != ESP_OK) return err;

    gpio_config_t dat = {
        .pin_bit_mask = (1ULL << PIN_DATA),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&dat);
    if (err != ESP_OK) return err;

    gpio_set_level(PIN_CLK, 0);
    gpio_set_level(PIN_DATA, 1);
    s_inited = true;
    ESP_LOGI(TAG, "AVR PDI init: CLK=GPIO%d DATA=GPIO%d", PIN_CLK, PIN_DATA);
    return ESP_OK;
}

/* ============================ Enable / belépés ======================== */

/* PDI engedélyezés: DATA magasban tartva >=16 CLK -> RESET-funkció letiltása. */
static esp_err_t pdi_enable(void)
{
    data_dir(true);
    gpio_set_level(PIN_DATA, 1);
    esp_rom_delay_us(100);
    for (int i = 0; i < 24; ++i) { clk_high(); clk_low(); }
    tx_idle(16);

    pdi_stcs(PDI_REG_CTRL, PDI_GT_CONSV);   /* guard time (óvatos) */

    uint8_t st = 0;
    esp_err_t err = pdi_ldcs(PDI_REG_STATUS, &st);
    if (err != ESP_OK) { ESP_LOGE(TAG, "PDI nem valaszol (STATUS)"); return err; }
    ESP_LOGI(TAG, "PDI elve, STATUS=%02X", st);
    return ESP_OK;
}

/* NVM engedélyezése: KEY 0xE0 + 8 kulcsbájt -> STATUS NVMEN. */
static esp_err_t pdi_enable_nvm(void)
{
    esp_err_t err = pdi_enable();
    if (err != ESP_OK) return err;

    uint8_t f[1] = { OP_KEY };
    pdi_send(f, sizeof(f));
    pdi_send(PDI_NVM_KEY, sizeof(PDI_NVM_KEY));

    for (int t = 0; t < 50; ++t) {
        uint8_t st = 0;
        if (pdi_ldcs(PDI_REG_STATUS, &st) == ESP_OK && (st & PDI_STATUS_NVMEN)) {
            ESP_LOGI(TAG, "NVMEN aktiv (STATUS=%02X)", st);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGE(TAG, "NVMEN nem allt be (rossz kulcs/bekotes?)");
    return ESP_FAIL;
}

/* Kilépés: PDI reset elengedése -> a cél fut. */
static void pdi_leave(void)
{
    pdi_stcs(PDI_REG_RESET, 0x00);
    tx_idle(8);
    data_dir(false);
    gpio_set_level(PIN_CLK, 0);
    ESP_LOGI(TAG, "PDI leave: cel elengedve");
}

/* Signature (MCU.DEVID 0x01000090..92). */
static esp_err_t pdi_read_signature(uint8_t out[3])
{
    for (int i = 0; i < 3; ++i) {
        esp_err_t err = pdi_lds8(MCU_DEVID0 + i, &out[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

/* Egy flash-lap programozása: buffer-erase -> load -> fill -> erase+write. */
static esp_err_t pdi_program_page(uint32_t page_addr, const uint8_t *page, uint16_t page_size)
{
    esp_err_t err;

    /* 1) Page buffer törlése (CMDEX-trigger). */
    pdi_nvm_cmd(NVMCMD_ERASE_FLASH_BUFFER);
    pdi_nvm_exec();
    if ((err = pdi_nvm_wait(50)) != ESP_OK) return err;

    /* 2) Page buffer feltöltése: LOAD_FLASH_BUFFER, majd ST-pointer + bájt-írások
     *    (minden flash-címre írás auto-tölti a buffert). */
    pdi_nvm_cmd(NVMCMD_LOAD_FLASH_BUFFER);
    pdi_st_ptr(page_addr);
    for (uint16_t b = 0; b < page_size; ++b) pdi_st_inc_byte(page[b]);

    /* 3) Erase+write page: CMD beállítás, majd dummy store a lap-címre triggerel. */
    pdi_nvm_cmd(NVMCMD_ERASE_WRITE_FLASH);
    pdi_st_ptr(page_addr);
    pdi_st_inc_byte(0x00);
    return pdi_nvm_wait(100);
}

/* ============================ Public: detect ========================== */
esp_err_t avr_pdi_detect(avr_pdi_dev_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_inited) { esp_err_t e = avr_pdi_init(); if (e != ESP_OK) return e; }
    memset(out, 0, sizeof(*out));

    esp_err_t err = pdi_enable();
    if (err != ESP_OK) return err;

    err = pdi_read_signature(out->sig);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Signature olvasas hiba"); pdi_leave(); return err; }
    ESP_LOGI(TAG, "Signature: %02X %02X %02X", out->sig[0], out->sig[1], out->sig[2]);

    const pdi_known_t *k = pdi_lookup(out->sig);
    if (k) {
        out->known = true; out->name = k->name;
        out->flash_size = k->flash_size; out->page_size = k->page_size;
        ESP_LOGI(TAG, "Eszkoz: %s (flash %u B, lap %u B)",
                 k->name, (unsigned)k->flash_size, (unsigned)k->page_size);
    } else {
        ESP_LOGW(TAG, "Ismeretlen PDI signature, nincs a tablaban");
    }

    pdi_leave();
    return ESP_OK;
}

/* ============================ Public: flash file ====================== */
esp_err_t avr_pdi_flash_file(const char *path, avr_pdi_progress_cb cb, void *ctx)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (!s_inited) { esp_err_t e = avr_pdi_init(); if (e != ESP_OK) return e; }

    void *raw = NULL;
    size_t raw_len = 0;
    esp_err_t err = storage_src_read_all(path, &raw, &raw_len);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Fajl olvasas hiba: %s (%s)", path, esp_err_to_name(err)); return err; }
    ESP_LOGI(TAG, "Forras: %s (%u B)", path, (unsigned)raw_len);

    uint8_t *img = NULL;
    esp_err_t ret = ESP_FAIL;

    err = pdi_enable_nvm();
    if (err != ESP_OK) { free(raw); return err; }

    uint8_t sig[3] = {0};
    pdi_read_signature(sig);
    ESP_LOGI(TAG, "Signature: %02X %02X %02X", sig[0], sig[1], sig[2]);
    const pdi_known_t *k = pdi_lookup(sig);
    if (!k) {
        ESP_LOGE(TAG, "Ismeretlen PDI signature — flashelest nem kockaztatok");
        ret = ESP_ERR_NOT_FOUND; goto out;
    }
    ESP_LOGI(TAG, "Eszkoz: %s (flash %u B, lap %u B)",
             k->name, (unsigned)k->flash_size, (unsigned)k->page_size);

    img = malloc(k->flash_size);
    if (!img) { ret = ESP_ERR_NO_MEM; goto out; }
    memset(img, 0xFF, k->flash_size);

    size_t img_len = 0;
    err = avr_image_parse(path, raw, raw_len, img, k->flash_size, &img_len);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Kep betoltes hiba (%s)", esp_err_to_name(err)); ret = err; goto out; }
    if (img_len == 0) { ESP_LOGW(TAG, "Ures kep, nincs mit irni"); ret = ESP_OK; goto out; }

    size_t prog_len = ((img_len + k->page_size - 1) / k->page_size) * k->page_size;
    if (prog_len > k->flash_size) prog_len = k->flash_size;
    size_t total_pages = prog_len / k->page_size;

    /* Lap-programozás (per lap erase+write -> nem kell külön chip erase, ami
     * amúgy is letiltaná az NVMEN-t). */
    if (cb) cb("Iras", 0, ctx);
    for (size_t p = 0; p < total_pages; ++p) {
        uint32_t page_addr = PDI_FLASH_BASE + (uint32_t)(p * k->page_size);
        if ((err = pdi_program_page(page_addr, &img[p * k->page_size], k->page_size)) != ESP_OK) {
            ESP_LOGE(TAG, "Lap-programozas hiba (lap %u)", (unsigned)p);
            ret = err; goto out;
        }
        int pct = (int)(((p + 1) * 100) / total_pages);
        if (cb) cb("Iras", pct, ctx);
    }
    ESP_LOGI(TAG, "Iras kesz: %u B", (unsigned)prog_len);

    /* Verify: READ_NVM mód + visszaolvasás. */
    pdi_nvm_cmd(NVMCMD_READ_NVM);
    for (size_t i = 0; i < prog_len; ++i) {
        uint8_t got;
        if ((err = pdi_lds8(PDI_FLASH_BASE + i, &got)) != ESP_OK) { ret = err; goto out; }
        if (got != img[i]) {
            ESP_LOGE(TAG, "Verify hiba @0x%lX: kapott %02X, vart %02X",
                     (unsigned long)i, got, img[i]);
            ret = ESP_FAIL; goto out;
        }
        if ((i & 0x7F) == 0 || i + 1 == prog_len) {
            int pct = (int)(((i + 1) * 100) / prog_len);
            if (cb) cb("Ellenor.", pct, ctx);
        }
    }
    pdi_nvm_cmd(NVMCMD_NO_OP);
    ESP_LOGI(TAG, "Verify OK: %u B egyezik", (unsigned)prog_len);
    ret = ESP_OK;

out:
    pdi_leave();
    if (img) free(img);
    free(raw);
    if (ret == ESP_OK) ESP_LOGI(TAG, "PDI flashelés kész: %s", path);
    return ret;
}
