/* SWD fizikai réteg (dedic_gpio HAL) — ESP32-S3.
 *
 * Megvalósítja a swd_phy.h primitíveit: a SWD bit-bankolást az S3 dedikált
 * GPIO perifériájával (driver/dedic_gpio.h) végezzük, ami ~single-cycle
 * írást/olvasást ad a sima gpio_set_level() helyett.
 *
 * Topológia (terv 2.2 / 5):
 *   - KIMENETI bundle: SWCLK + SWDIO   -> órajel + adatmeghajtás
 *   - BEMENETI  bundle: SWDIO          -> turnaround utáni olvasás
 * A SWDIO ugyanaz a pad mindkét bundle-ben: a TÉNYLEGES irányt a pad
 * mód (output-engedélyezve / input él) dönti el, NEM a bundle önmagában.
 * Ezért swd_phy_dir() a pad in/out engedélyezőit kapcsolgatja.
 */

#include "swd_phy.h"

#include "driver/gpio.h"
#include "driver/dedic_gpio.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "esp_rom_sys.h"      /* esp_rom_get_cpu_ticks_per_us */
#include "esp_log.h"

/* ======================= Lábkiosztás (könnyen átírható) ===================== */
#define PIN_SWCLK   4   /* SWCLK kimenet */
#define PIN_SWDIO   5   /* SWDIO kétirányú (turnaround) */
#define PIN_NRST    6   /* nRST open-drain (assert=alacsony) */

/* ============== dedic_gpio bundle bitmaszkok (a bundle-en belüli pozíció) ====
 * A kimeneti bundle tömbjében a 0. elem = SWCLK, 1. elem = SWDIO,
 * így a bundle-be írt érték 0. bitje SWCLK, 1. bitje SWDIO.
 * A bemeneti bundle egyetlen eleme a SWDIO -> 0. bit. */
#define SWCLK_MASK     (1u << 0)
#define SWDIO_MASK     (1u << 1)
#define IN_SWDIO_MASK  (1u << 0)

/* ============================ Modul-állapot ================================ */
static dedic_gpio_bundle_handle_t s_out;  /* SWCLK+SWDIO kimeneti bundle */
static dedic_gpio_bundle_handle_t s_in;   /* SWDIO bemeneti bundle */

/* Fél-órajel késleltetés ciklusokban (NOP-pörgetés). 0 = nincs extra delay. */
static volatile uint32_t s_half_cycle_loops;

/* ===================== Alacsony szintű órajel-segédek ====================== *
 * Az SWD a SWCLK FELFUTÓ élén mintavételez: egy bit kiküldésének rendje
 * mindig SWDIO-beállítás -> clk_low -> clk_high. */

static inline void delay_half(void)
{
    /* Best-effort lassítás: rövid pörgő ciklus. Bring-up alatt ~200-500 kHz-re
     * elég; nagy pontosságot nem ígérünk (lásd swd_phy_set_freq_hz). */
    uint32_t n = s_half_cycle_loops;
    while (n--) {
        __asm__ __volatile__("nop");
    }
}

static inline void clk_low(void)
{
    dedic_gpio_bundle_write(s_out, SWCLK_MASK, 0);
}

static inline void clk_high(void)
{
    dedic_gpio_bundle_write(s_out, SWCLK_MASK, SWCLK_MASK);
}

/* ============================== init ====================================== */

esp_err_t swd_phy_init(void)
{
    /* --- SWCLK + SWDIO padok GPIO-ra állítása ---
     * SWDIO INPUT_OUTPUT, hogy a bemeneti út (turnaround) is élő legyen. */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_SWCLK) | (1ULL << PIN_SWDIO),
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,      /* SWDIO nyugalmi szint magas */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    /* nRST: open-drain, induláskor felengedve (nincs reset). Külső pullup húz. */
    gpio_config_t nrst = {
        .pin_bit_mask = (1ULL << PIN_NRST),
        .mode         = GPIO_MODE_OUTPUT_OD,     /* open-drain */
        .pull_up_en   = GPIO_PULLUP_ENABLE,      /* belső pullup is segít felfelé */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&nrst);
    gpio_set_level(PIN_NRST, 1);                 /* OD + 1 = magas impedancia */

    /* --- Kimeneti bundle: [0]=SWCLK, [1]=SWDIO --- */
    const int out_pins[] = { PIN_SWCLK, PIN_SWDIO };
    dedic_gpio_bundle_config_t out_cfg = {
        .gpio_array = out_pins,
        .array_size = sizeof(out_pins) / sizeof(out_pins[0]),
        .flags = { .out_en = 1 },
    };
    esp_err_t err = dedic_gpio_new_bundle(&out_cfg, &s_out);
    if (err != ESP_OK) {
        return err;
    }

    /* --- Bemeneti bundle: [0]=SWDIO --- */
    const int in_pins[] = { PIN_SWDIO };
    dedic_gpio_bundle_config_t in_cfg = {
        .gpio_array = in_pins,
        .array_size = sizeof(in_pins) / sizeof(in_pins[0]),
        .flags = { .in_en = 1 },
    };
    err = dedic_gpio_new_bundle(&in_cfg, &s_in);
    if (err != ESP_OK) {
        return err;
    }

    /* --- KRITIKUS a turnaroundhoz: a SWDIO output-enable forrása ---
     * A dedic_gpio a kimeneti jelet a GPIO-mátrixon át vezeti, és a pad
     * output-enable-jét ALAPÉRTELMEZÉSBEN a dedic periféria adja (mindig
     * hajt). Emiatt a gpio_ll_output_disable() a swd_phy_dir(false)-ben nem
     * engedi el a vonalat -> a cél nem tud lehúzni -> csupa-1 ACK (0x7).
     * Megoldás: oen_sel=1 a SWDIO padra, így az OEN a GPIO_ENABLE regiszterből
     * jön (amit a gpio_ll_output_enable/disable állít), míg az ADAT továbbra is
     * a dedic kimenetről. A SWCLK-t NEM bántjuk (az végig hajtott). */
    GPIO.func_out_sel_cfg[PIN_SWDIO].oen_sel = 1;

    /* Biztonságos alap: lassú baud, SWDIO meghajtva magasra, SWCLK alacsony. */
    swd_phy_set_freq_hz(300000);   /* ~300 kHz bring-up baud */
    swd_phy_dir(true);
    dedic_gpio_bundle_write(s_out, SWDIO_MASK, SWDIO_MASK);
    clk_low();

    return ESP_OK;
}

/* ============================ irány (turnaround) =========================== *
 * A SWDIO egyetlen pad, de két bundle-ben szerepel. A dedic_gpio kimeneti
 * útja a pad output-engedélyezőjén keresztül hat: ha a padot kimenetileg
 * letiltjuk, a kimeneti bundle írása nem jut ki a vezetékre, viszont a
 * bemeneti bundle olvasása érvényes (a céleszköz hajtja a vonalat).
 * Ezért a turnaroundnál a pad output-engedélyezőjét váltjuk.
 *   drive=true  -> SWDIO output engedélyezve (mi hajtjuk meg)
 *   drive=false -> SWDIO output letiltva, input él (a cél hajtja) */
void swd_phy_dir(bool drive)
{
    if (drive) {
        /* Output engedélyezése; az input út INPUT_OUTPUT módban végig él. */
        gpio_ll_output_enable(&GPIO, PIN_SWDIO);
    } else {
        /* Output letiltása -> kimenetileg magas-Z; input olvas. */
        gpio_ll_output_disable(&GPIO, PIN_SWDIO);
        gpio_ll_input_enable(&GPIO, PIN_SWDIO);
    }
}

/* ============================ kiküldés (LSB-first) ========================= *
 * Minden bit: SWDIO beállít -> clk_low -> clk_high (cél a felfutó élen mintáz).
 * A bitek LSB-first sorrendben mennek ki (i=0 a legalsó bit).
 * Hívás előtt SWDIO legyen meghajtott irányban: swd_phy_dir(true). */
void swd_phy_seq_out(uint32_t bits, int n)
{
    for (int i = 0; i < n; i++) {
        uint32_t v = ((bits >> i) & 1u) ? SWDIO_MASK : 0;
        dedic_gpio_bundle_write(s_out, SWDIO_MASK, v);
        clk_low();
        delay_half();
        clk_high();
        delay_half();
    }
}

/* ============================ beolvasás (LSB-first) ======================== *
 * A cél a SWDIO-t a SWCLK felfutó éléhez igazítja; mi a magas fázisban
 * (az él után, stabil adatnál) mintavételezünk. A hívó előtte turnaroundot
 * tett: swd_phy_dir(false). Az eredmény bitjei LSB-first épülnek (i=0 alul). */
uint32_t swd_phy_seq_in(int n)
{
    uint32_t out = 0;
    for (int i = 0; i < n; i++) {
        clk_low();
        delay_half();
        /* Mintavétel a LOW fázis végén, a felfutó él ELŐTT.
         * A cél a felfutó élen VÁLT a következő bitre (ahogy mi is a write-on),
         * így az aktuális bit a low fázisban stabil, és a felfutó élen lenne
         * mintázandó. Ha a clk_high() UTÁN mintáznánk, a cél már a következő
         * bitre váltott -> 1 bit csúszás, az ACK LSB elveszik -> 0x0. */
        uint32_t r = dedic_gpio_bundle_read_in(s_in);
        if (r & IN_SWDIO_MASK) {
            out |= (1u << i);
        }
        clk_high();
        delay_half();
    }
    return out;
}

/* ============================ idle / line-reset =========================== *
 * SWDIO=1 tartva `clocks` órajelig. Idle ciklusokhoz és line-reset-hez.
 * Feltételezi, hogy SWDIO meghajtott (drive) irányban van. */
void swd_phy_idle(int clocks)
{
    dedic_gpio_bundle_write(s_out, SWDIO_MASK, SWDIO_MASK);  /* SWDIO magas */
    for (int i = 0; i < clocks; i++) {
        clk_low();
        delay_half();
        clk_high();
        delay_half();
    }
}

/* ============================ frekvencia (best-effort) ===================== *
 * Egyszerű NOP-ciklus alapú fél-órajel késleltetés kalibrálása. Nem precíz:
 * a dedic_gpio írások és a ciklusfej overheadje miatt a tényleges baud
 * valamivel alacsonyabb lesz. hz=0 -> nincs extra delay (leggyorsabb). */
void swd_phy_set_freq_hz(uint32_t hz)
{
    if (hz == 0) {
        s_half_cycle_loops = 0;
        return;
    }

    /* Egy teljes SWD órajel = 2 fél-periódus (low + high). A fél-periódus
     * ideje = 1/(2*hz). NOP-ciklus durva becslése (1 nop/iteráció):
     *   loops ~= cpu_hz / (2*hz). Csak közelítés bring-up-hoz. */
    uint32_t cpu_hz = (uint32_t)esp_rom_get_cpu_ticks_per_us() * 1000000u;
    uint32_t half = cpu_hz / (2u * hz);

    /* Fix overhead levonása (dedic write + ciklusfej, durván ~10 ciklus),
     * hogy alacsony frekvencián ne lassítsunk túl. */
    const uint32_t overhead = 10;
    s_half_cycle_loops = (half > overhead) ? (half - overhead) : 0;
}

/* ============================ IO-önteszt (diagnosztika) ==================== *
 * A protokoll által ténylegesen használt bemeneti utat (s_in dedic bundle)
 * teszteli loopback-kel: meghajtjuk a SWDIO padot és visszaolvassuk. Ezzel
 * eldönthető, hogy a csupa-1 ACK valódi cél-hiány-e, vagy a read-út hibája. */

static const char *PHY_TAG = "swd_phy";

/* A protokollal AZONOS úton olvas: a bemeneti dedic bundle SWDIO bitje. */
static inline int read_swdio(void)
{
    return (dedic_gpio_bundle_read_in(s_in) & IN_SWDIO_MASK) ? 1 : 0;
}

bool swd_phy_selftest_io(void)
{
    /* 1) Released: magas-Z, csak a felhúzó húz. Elvárt: 1. */
    swd_phy_dir(false);
    esp_rom_delay_us(5);
    int rel = read_swdio();

    /* 2) Meghajtva 0. Elvárt: 0. (Ha 1, a bemeneti út nem látja a padot.) */
    swd_phy_dir(true);
    dedic_gpio_bundle_write(s_out, SWDIO_MASK, 0);
    esp_rom_delay_us(5);
    int lo = read_swdio();

    /* 3) Meghajtva 1. Elvárt: 1. */
    dedic_gpio_bundle_write(s_out, SWDIO_MASK, SWDIO_MASK);
    esp_rom_delay_us(5);
    int hi = read_swdio();

    /* 4) TRI-STATE próba: dir(false) UTÁN a padon csak belső pull-down.
     *    Ha a kimenet tényleg el van engedve, a pull-down lehúz -> 0.
     *    Ha az ESP MÉG MINDIG hajtja magasra (a dir(false) nem tri-state-el),
     *    akkor 1 marad -> ez a csupa-1 ACK valódi oka (a cél nem tud lehúzni). */
    dedic_gpio_bundle_write(s_out, SWDIO_MASK, SWDIO_MASK);  /* belső latch magas */
    swd_phy_dir(false);                                      /* "elengedés" */
    gpio_set_pull_mode(PIN_SWDIO, GPIO_PULLDOWN_ONLY);
    esp_rom_delay_us(20);
    int released_pd = read_swdio();
    gpio_set_pull_mode(PIN_SWDIO, GPIO_PULLUP_ONLY);         /* eredeti felhúzó vissza */

    /* Néhány látható SWCLK pulzus analizátorhoz (SWDIO közben magas). */
    swd_phy_dir(true);
    dedic_gpio_bundle_write(s_out, SWDIO_MASK, SWDIO_MASK);
    for (int i = 0; i < 8; i++) { clk_low(); delay_half(); clk_high(); delay_half(); }

    ESP_LOGI(PHY_TAG, "[IO-TEST] released=%d (var:1)  drive_lo=%d (var:0)  drive_hi=%d (var:1)  tristate_pd=%d (var:0)",
             rel, lo, hi, released_pd);

    bool ok = (rel == 1) && (lo == 0) && (hi == 1) && (released_pd == 0);
    if (!ok) {
        if (released_pd == 1) {
            ESP_LOGW(PHY_TAG, "[IO-TEST] HIBA: dir(false) utan a pad pull-down ellenere 1 -> "
                              "a SWDIO NEM tri-state, az ESP tovabbra is hajtja magasra. "
                              "A cel nem tud lehuzni -> ez a csupa-1 ACK valodi (SZOFTVERES) oka!");
        } else if (lo == 1) {
            ESP_LOGW(PHY_TAG, "[IO-TEST] HIBA: meghajtott 0-t 1-nek olvas -> a bemeneti "
                              "(dedic input) ut NEM latja a SWDIO pad valos szintjet.");
        } else {
            ESP_LOGW(PHY_TAG, "[IO-TEST] HIBA: varatlan olvasat (rel=%d lo=%d hi=%d pd=%d).",
                     rel, lo, hi, released_pd);
        }
    } else {
        ESP_LOGI(PHY_TAG, "[IO-TEST] OK: read-ut + tri-state is helyes -> a csupa-1 ACK "
                          "valodi cel-hiany/bekotes problema (nem szoftver).");
    }

    /* Biztonságos alap visszaállítása. */
    swd_phy_dir(true);
    dedic_gpio_bundle_write(s_out, SWDIO_MASK, SWDIO_MASK);
    clk_low();
    return ok;
}

/* ============================ szint-olvasás (diag) ======================== */
int swd_phy_read_level(void)
{
    return (dedic_gpio_bundle_read_in(s_in) & IN_SWDIO_MASK) ? 1 : 0;
}

/* ============================ nRST (open-drain) =========================== *
 * assert=true  -> 0 szint kihúzva (reset aktív)
 * assert=false -> magas impedancia, a külső (és belső) pullup húz fel.
 * A pad OD módban van (init): level=1 már magas-Z-t ad; ezt tovább erősítjük
 * az output-engedélyező letiltásával, hogy biztosan ne hajtsuk felfelé. */
void swd_phy_nrst(bool assert)
{
    if (assert) {
        gpio_ll_output_enable(&GPIO, PIN_NRST);
        gpio_set_level(PIN_NRST, 0);              /* aktívan lehúz 0-ra */
    } else {
        gpio_set_level(PIN_NRST, 1);              /* OD: magas-Z */
        gpio_ll_output_disable(&GPIO, PIN_NRST);  /* biztos magas impedancia */
    }
}
