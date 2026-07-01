/* AVR TPI programozó (reduced-core ATtiny) — bit-bang PHY + TPI link + NVM.
 *
 * Réteg-felépítés (alulról):
 *   PHY     : bit-bang TPICLK (kimenet) + TPIDATA (kétirányú) + RESET (alacsony).
 *             12-bites keret: start(0) + 8 adat LSB-first + páros paritás + 2 stop(1).
 *             NINCS SYNC-bájt (mint a PDI); a turnaround guard-idle bitekkel megy.
 *   Link    : SLDCS/SSTCS/SKEY/SSTPR/SST(+)/SLD(+)/SIN/SOUT.
 *   NVM     : reduced-core tiny NVM-vezérlő (WORD/DWORD/CODE_WRITE, CHIP_ERASE);
 *             a magas-bájt tárolása triggerel.
 *   Orchestr: enable (RESET low + 16 CLK) -> SKEY -> NVMEN -> chip erase ->
 *             word program -> verify -> reset elenged.
 *
 * A konstansok az avrdude forráshoz (src/tpi.h, src/avr.c) + Atmel AVR918
 * (doc8373) app note-hoz igazítva. HW-n MÉG NEM IGAZOLT. Lásd: reference/AVR_TPI.md.
 */
#include "avr_tpi.h"

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

static const char *TAG = "avr_tpi";

/* ============================ Lábkiosztás (Kconfig) ===================== */
#define PIN_CLK   CONFIG_AVR_TPI_CLK_GPIO     /* TPICLK  (default 15 = SCK) */
#define PIN_DATA  CONFIG_AVR_TPI_DATA_GPIO    /* TPIDATA (default 7 = MISO) */
#define PIN_RESET CONFIG_AVR_TPI_RESET_GPIO   /* RESET   (default 21) */

#define TPI_HALF_US   3                       /* ~150 kHz TPICLK; szinkron -> OK */

static bool s_inited = false;

/* ============================ TPI link konstansok ====================== */
/* Utasítás-opkódok (avrdude src/tpi.h). */
#define CMD_SLD     0x20
#define CMD_SLD_INC 0x24
#define CMD_SST     0x60
#define CMD_SST_INC 0x64
#define CMD_SSTPR   0x68   /* |0 = low bájt, |1 = high bájt */
#define CMD_SIN     0x10   /* | SIO_ADDR(io) */
#define CMD_SOUT    0x90   /* | SIO_ADDR(io) */
#define CMD_SLDCS   0x80   /* | css_index */
#define CMD_SSTCS   0xC0   /* | css_index */
#define CMD_SKEY    0xE0

/* 6-bites I/O-cím kódolása a SIN/SOUT opkódba. */
#define SIO_ADDR(x)  ((((x) & 0x30) << 1) | ((x) & 0x0F))

/* Control/Status (CSS) regiszterek. */
#define TPI_TPISR   0x00   /* NVMEN = bit1 */
#define TPI_TPIPCR  0x02   /* guard time */
#define TPI_TPIIR   0x0F   /* ident, elvárt 0x80 */
#define TPISR_NVMEN 0x02
#define TPI_IDENT   0x80
#define TPIPCR_GUARD 0x07  /* min guard (2 idle bit) — avrdude is leszorítja */

/* NVM enable kulcs (0x1289AB45CDD888FF) — a vonalra FORDÍTVA (avrdude tpi_skey_cmd). */
static const uint8_t TPI_SKEY[8] = { 0xFF, 0x88, 0xD8, 0xCD, 0x45, 0xAB, 0x89, 0x12 };

/* NVM-vezérlő I/O-regiszterek (reduced-core tiny). */
#define NVMCSR_IO   0x32   /* NVMBSY = bit7 */
#define NVMCMD_IO   0x33
#define NVMCSR_BSY  0x80

/* NVM parancsok. */
#define NVMCMD_NO_OP     0x00
#define NVMCMD_CHIP_ERASE 0x10
#define NVMCMD_SECT_ERASE 0x14
#define NVMCMD_WORD_WRITE 0x1D   /* tiny4/5/9/10: WORD; tiny20: DWORD; tiny40: CODE */

/* Flash a TPI data-térben 0x4000-nál; signature-sor 0x3FC0-nál. */
#define TPI_FLASH_BASE  0x4000
#define TPI_SIGROW      0x3FC0

/* ============================ Cél-tábla ================================ */
typedef struct {
    uint8_t  sig[3];
    const char *name;
    uint32_t flash_size;   /* bájt */
    uint16_t page_size;    /* bájt (szekció-erase granularitás) */
    uint8_t  block_words;  /* egy NVM-write triggerhez szükséges szavak (WORD=1/DWORD=2/CODE=4) */
} tpi_known_t;

/* avrdude.conf szerint (signature/flash/lap); block_words HW-n ellenőrizendő. */
static const tpi_known_t TPI_TABLE[] = {
    { { 0x1E, 0x8F, 0x0A }, "ATtiny4",   0x200,  16, 1 },
    { { 0x1E, 0x8F, 0x09 }, "ATtiny5",   0x200,  16, 1 },
    { { 0x1E, 0x90, 0x08 }, "ATtiny9",   0x400,  16, 1 },
    { { 0x1E, 0x90, 0x03 }, "ATtiny10",  0x400,  16, 1 },
    { { 0x1E, 0x90, 0x0C }, "ATtiny102", 0x400,  16, 1 },
    { { 0x1E, 0x90, 0x0B }, "ATtiny104", 0x400,  16, 1 },
    { { 0x1E, 0x91, 0x0F }, "ATtiny20",  0x800,  16, 2 },
    { { 0x1E, 0x92, 0x0E }, "ATtiny40",  0x1000, 64, 4 },
};

static const tpi_known_t *tpi_lookup(const uint8_t sig[3])
{
    for (size_t i = 0; i < sizeof(TPI_TABLE) / sizeof(TPI_TABLE[0]); ++i) {
        if (memcmp(TPI_TABLE[i].sig, sig, 3) == 0) return &TPI_TABLE[i];
    }
    return NULL;
}

/* ============================ PHY (bit-bang) =========================== */

static inline void clk_high(void) { gpio_set_level(PIN_CLK, 1); esp_rom_delay_us(TPI_HALF_US); }
static inline void clk_low(void)  { gpio_set_level(PIN_CLK, 0); esp_rom_delay_us(TPI_HALF_US); }

static void data_dir(bool drive)
{
    gpio_set_direction(PIN_DATA, drive ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
}

/* Egy bit ki: adat beáll, felfutó él (cél mintáz), lefutó él. */
static void tx_bit(int bit)
{
    gpio_set_level(PIN_DATA, bit & 1);
    esp_rom_delay_us(TPI_HALF_US);
    clk_high();
    clk_low();
}

static int rx_bit(void)
{
    gpio_set_level(PIN_CLK, 1);
    esp_rom_delay_us(TPI_HALF_US);
    int b = gpio_get_level(PIN_DATA);
    gpio_set_level(PIN_CLK, 0);
    esp_rom_delay_us(TPI_HALF_US);
    return b;
}

/* Idle bitek (DATA=1). */
static void tx_idle(int bits)
{
    gpio_set_level(PIN_DATA, 1);
    for (int i = 0; i < bits; ++i) { clk_high(); clk_low(); }
}

/* 12-bites keret: start(0) + 8 adat LSB-first + páros paritás + 2 stop(1). */
static void tpi_tx_byte(uint8_t b)
{
    int par = 0;
    tx_bit(0);
    for (int i = 0; i < 8; ++i) { int bit = (b >> i) & 1; par ^= bit; tx_bit(bit); }
    tx_bit(par);
    tx_bit(1); tx_bit(1);
}

static esp_err_t tpi_rx_byte(uint8_t *out)
{
    int found = 0;
    for (int n = 0; n < 64; ++n) { if (rx_bit() == 0) { found = 1; break; } }
    if (!found) { ESP_LOGW(TAG, "RX: nincs start-bit (timeout)"); return ESP_ERR_TIMEOUT; }
    int par = 0; uint8_t v = 0;
    for (int i = 0; i < 8; ++i) { int bit = rx_bit(); par ^= bit; v |= (bit << i); }
    int pbit = rx_bit();
    int s1 = rx_bit(); (void)rx_bit();
    if (pbit != par) { ESP_LOGW(TAG, "RX: paritashiba (%02X)", v); return ESP_FAIL; }
    if (s1 != 1)     { ESP_LOGW(TAG, "RX: framing (stop=%d)", s1); return ESP_FAIL; }
    *out = v;
    return ESP_OK;
}

/* Küldés (mi hajtjuk): elé 2 idle bit (a programozónak >=1 idle kell TX előtt). */
static void tpi_send(const uint8_t *buf, size_t n)
{
    data_dir(true);
    tx_idle(2);
    for (size_t i = 0; i < n; ++i) tpi_tx_byte(buf[i]);
}

static esp_err_t tpi_recv(uint8_t *buf, size_t n)
{
    data_dir(false);
    for (size_t i = 0; i < n; ++i) {
        esp_err_t err = tpi_rx_byte(&buf[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

/* ============================ Link-utasítások ========================= */

static esp_err_t tpi_sldcs(uint8_t css, uint8_t *out)
{
    uint8_t f[1] = { (uint8_t)(CMD_SLDCS | (css & 0x0F)) };
    tpi_send(f, 1);
    return tpi_recv(out, 1);
}

static void tpi_sstcs(uint8_t css, uint8_t val)
{
    uint8_t f[2] = { (uint8_t)(CMD_SSTCS | (css & 0x0F)), val };
    tpi_send(f, 2);
}

/* I/O-regiszter olvasás/írás (SIN/SOUT). */
static esp_err_t tpi_sin(uint8_t io, uint8_t *out)
{
    uint8_t f[1] = { (uint8_t)(CMD_SIN | SIO_ADDR(io)) };
    tpi_send(f, 1);
    return tpi_recv(out, 1);
}

static void tpi_sout(uint8_t io, uint8_t val)
{
    uint8_t f[2] = { (uint8_t)(CMD_SOUT | SIO_ADDR(io)), val };
    tpi_send(f, 2);
}

/* Pointer-regiszter beállítása (16-bit). */
static void tpi_sstpr(uint16_t addr)
{
    uint8_t f[4] = { (uint8_t)(CMD_SSTPR | 0), (uint8_t)(addr & 0xFF),
                     (uint8_t)(CMD_SSTPR | 1), (uint8_t)((addr >> 8) & 0xFF) };
    tpi_send(f, 4);
}

static void tpi_sst(uint8_t data)      { uint8_t f[2] = { CMD_SST, data };     tpi_send(f, 2); }
static void tpi_sst_inc(uint8_t data)  { uint8_t f[2] = { CMD_SST_INC, data }; tpi_send(f, 2); }

static esp_err_t tpi_sld_inc(uint8_t *out)
{
    uint8_t f[1] = { CMD_SLD_INC };
    tpi_send(f, 1);
    return tpi_recv(out, 1);
}

static void tpi_skey(void)
{
    uint8_t f[1] = { CMD_SKEY };
    tpi_send(f, 1);
    tpi_send(TPI_SKEY, sizeof(TPI_SKEY));
}

/* NVMBSY poll (SIN NVMCSR, bit7). */
static esp_err_t tpi_nvm_wait(int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    do {
        uint8_t st;
        if (tpi_sin(NVMCSR_IO, &st) == ESP_OK && (st & NVMCSR_BSY) == 0) return ESP_OK;
        esp_rom_delay_us(200);
    } while (esp_timer_get_time() < deadline);
    ESP_LOGE(TAG, "NVMBSY timeout");
    return ESP_ERR_TIMEOUT;
}

/* ============================ GPIO init =============================== */
esp_err_t avr_tpi_init(void)
{
    if (s_inited) return ESP_OK;
    gpio_config_t o = {
        .pin_bit_mask = (1ULL << PIN_CLK) | (1ULL << PIN_RESET),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&o);
    if (err != ESP_OK) return err;

    gpio_config_t d = {
        .pin_bit_mask = (1ULL << PIN_DATA),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&d);
    if (err != ESP_OK) return err;

    gpio_set_level(PIN_CLK, 0);
    gpio_set_level(PIN_DATA, 1);
    gpio_set_level(PIN_RESET, 1);   /* idle: cél fut */
    s_inited = true;
    ESP_LOGI(TAG, "AVR TPI init: CLK=GPIO%d DATA=GPIO%d RESET=GPIO%d",
             PIN_CLK, PIN_DATA, PIN_RESET);
    return ESP_OK;
}

/* ============================ Enable / belépés ======================== */

/* TPI engedélyezés: RESET alacsony + TPIDATA magasban >=16 TPICLK. */
static esp_err_t tpi_enable(void)
{
    data_dir(true);
    gpio_set_level(PIN_DATA, 1);
    gpio_set_level(PIN_RESET, 0);        /* RESET aktív: a TPI PHY bekapcsol */
    esp_rom_delay_us(200);
    tx_idle(32);                         /* >=16 CLK DATA=1 -> TPI aktiv */

    tpi_sstcs(TPI_TPIPCR, TPIPCR_GUARD); /* guard time leszorítás */

    uint8_t id = 0;
    esp_err_t err = tpi_sldcs(TPI_TPIIR, &id);
    if (err != ESP_OK) { ESP_LOGE(TAG, "TPI nem valaszol (TPIIR)"); return err; }
    if (id != TPI_IDENT) ESP_LOGW(TAG, "TPIIR=%02X (vart 0x80)", id);
    else ESP_LOGI(TAG, "TPI elve (TPIIR=0x80)");
    return ESP_OK;
}

/* NVM engedélyezése: SKEY + kulcs -> TPISR NVMEN. */
static esp_err_t tpi_enable_nvm(void)
{
    esp_err_t err = tpi_enable();
    if (err != ESP_OK) return err;

    tpi_skey();
    for (int t = 0; t < 20; ++t) {
        uint8_t sr = 0;
        if (tpi_sldcs(TPI_TPISR, &sr) == ESP_OK && (sr & TPISR_NVMEN)) {
            ESP_LOGI(TAG, "NVMEN aktiv (TPISR=%02X)", sr);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGE(TAG, "NVMEN nem allt be (rossz kulcs/bekotes?)");
    return ESP_FAIL;
}

/* Kilépés: NVMEN törlés + RESET elenged. */
static void tpi_leave(void)
{
    tpi_sstcs(TPI_TPISR, 0x00);
    tx_idle(8);
    data_dir(false);
    gpio_set_level(PIN_CLK, 0);
    gpio_set_level(PIN_RESET, 1);   /* cél fut */
    ESP_LOGI(TAG, "TPI leave: cel elengedve");
}

/* Signature 3 bájt a SIGROW-ból (SSTPR + SLD+). */
static esp_err_t tpi_read_signature(uint8_t out[3])
{
    tpi_sstpr(TPI_SIGROW);
    for (int i = 0; i < 3; ++i) {
        esp_err_t err = tpi_sld_inc(&out[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

/* Chip erase: NVMCMD=CHIP_ERASE, majd dummy store a magas-bájtra triggerel. */
static esp_err_t tpi_chip_erase(void)
{
    tpi_sstpr((uint16_t)(TPI_FLASH_BASE | 1));   /* magas-bájtra mutat */
    tpi_sout(NVMCMD_IO, NVMCMD_CHIP_ERASE);
    tpi_sst(0xFF);                               /* dummy store triggerel */
    return tpi_nvm_wait(100);
}

/* Egy blokknyi (block_words szó) írása -> a blokk utolsó magas-bájtja triggerel. */
static esp_err_t tpi_write_block(uint16_t addr, const uint8_t *data, uint8_t block_words)
{
    tpi_sout(NVMCMD_IO, NVMCMD_WORD_WRITE);
    tpi_sstpr(addr);
    for (uint8_t w = 0; w < block_words; ++w) {
        tpi_sst_inc(data[2 * w]);        /* low bájt */
        tpi_sst_inc(data[2 * w + 1]);    /* high bájt (az utolsó triggerel) */
        if (w != block_words - 1) tx_idle(2);  /* idle karakter a szavak közt */
    }
    return tpi_nvm_wait(50);
}

/* ============================ Public: detect ========================== */
esp_err_t avr_tpi_detect(avr_tpi_dev_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_inited) { esp_err_t e = avr_tpi_init(); if (e != ESP_OK) return e; }
    memset(out, 0, sizeof(*out));

    esp_err_t err = tpi_enable();
    if (err != ESP_OK) return err;

    err = tpi_read_signature(out->sig);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Signature olvasas hiba"); tpi_leave(); return err; }
    ESP_LOGI(TAG, "Signature: %02X %02X %02X", out->sig[0], out->sig[1], out->sig[2]);

    const tpi_known_t *k = tpi_lookup(out->sig);
    if (k) {
        out->known = true; out->name = k->name;
        out->flash_size = k->flash_size; out->page_size = k->page_size;
        ESP_LOGI(TAG, "Eszkoz: %s (flash %u B, lap %u B)",
                 k->name, (unsigned)k->flash_size, (unsigned)k->page_size);
    } else {
        ESP_LOGW(TAG, "Ismeretlen TPI signature, nincs a tablaban");
    }

    tpi_leave();
    return ESP_OK;
}

/* ============================ Public: flash file ====================== */
esp_err_t avr_tpi_flash_file(const char *path, avr_tpi_progress_cb cb, void *ctx)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (!s_inited) { esp_err_t e = avr_tpi_init(); if (e != ESP_OK) return e; }

    void *raw = NULL;
    size_t raw_len = 0;
    esp_err_t err = storage_src_read_all(path, &raw, &raw_len);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Fajl olvasas hiba: %s (%s)", path, esp_err_to_name(err)); return err; }
    ESP_LOGI(TAG, "Forras: %s (%u B)", path, (unsigned)raw_len);

    uint8_t *img = NULL;
    esp_err_t ret = ESP_FAIL;

    err = tpi_enable_nvm();
    if (err != ESP_OK) { free(raw); return err; }

    uint8_t sig[3] = {0};
    tpi_read_signature(sig);
    ESP_LOGI(TAG, "Signature: %02X %02X %02X", sig[0], sig[1], sig[2]);
    const tpi_known_t *k = tpi_lookup(sig);
    if (!k) {
        ESP_LOGE(TAG, "Ismeretlen TPI signature — flashelest nem kockaztatok");
        ret = ESP_ERR_NOT_FOUND; goto out;
    }
    ESP_LOGI(TAG, "Eszkoz: %s (flash %u B, lap %u B, blokk %u szo)",
             k->name, (unsigned)k->flash_size, (unsigned)k->page_size, k->block_words);

    img = malloc(k->flash_size);
    if (!img) { ret = ESP_ERR_NO_MEM; goto out; }
    memset(img, 0xFF, k->flash_size);

    size_t img_len = 0;
    err = avr_image_parse(path, raw, raw_len, img, k->flash_size, &img_len);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Kep betoltes hiba (%s)", esp_err_to_name(err)); ret = err; goto out; }
    if (img_len == 0) { ESP_LOGW(TAG, "Ures kep, nincs mit irni"); ret = ESP_OK; goto out; }

    /* Blokkra (block_words*2 bájt) kerekítés. */
    size_t block_bytes = (size_t)k->block_words * 2;
    size_t prog_len = ((img_len + block_bytes - 1) / block_bytes) * block_bytes;
    if (prog_len > k->flash_size) prog_len = k->flash_size;

    /* Chip erase. */
    if (cb) cb("Torles", 0, ctx);
    if ((err = tpi_chip_erase()) != ESP_OK) { ret = err; goto out; }
    if (cb) cb("Torles", 100, ctx);

    /* Word/DWORD/CODE program blokkonként. */
    if (cb) cb("Iras", 0, ctx);
    for (size_t off = 0; off < prog_len; off += block_bytes) {
        if ((err = tpi_write_block((uint16_t)(TPI_FLASH_BASE + off),
                                   &img[off], k->block_words)) != ESP_OK) {
            ESP_LOGE(TAG, "Blokk-iras hiba (off 0x%X)", (unsigned)off);
            ret = err; goto out;
        }
        int pct = (int)(((off + block_bytes) * 100) / prog_len);
        if (cb) cb("Iras", pct > 100 ? 100 : pct, ctx);
    }
    ESP_LOGI(TAG, "Iras kesz: %u B", (unsigned)prog_len);

    /* Verify: SSTPR + SLD+ végig. */
    tpi_sstpr(TPI_FLASH_BASE);
    for (size_t i = 0; i < prog_len; ++i) {
        uint8_t got;
        if ((err = tpi_sld_inc(&got)) != ESP_OK) { ret = err; goto out; }
        if (got != img[i]) {
            ESP_LOGE(TAG, "Verify hiba @0x%X: kapott %02X, vart %02X",
                     (unsigned)(TPI_FLASH_BASE + i), got, img[i]);
            ret = ESP_FAIL; goto out;
        }
        if ((i & 0x3F) == 0 || i + 1 == prog_len) {
            int pct = (int)(((i + 1) * 100) / prog_len);
            if (cb) cb("Ellenor.", pct, ctx);
        }
    }
    ESP_LOGI(TAG, "Verify OK: %u B egyezik", (unsigned)prog_len);
    ret = ESP_OK;

out:
    tpi_leave();
    if (img) free(img);
    free(raw);
    if (ret == ESP_OK) ESP_LOGI(TAG, "TPI flashelés kész: %s", path);
    return ret;
}
