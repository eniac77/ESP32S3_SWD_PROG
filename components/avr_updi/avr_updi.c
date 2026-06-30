/* AVR UPDI programozó — UART single-wire (8E2) + NVMCTRL-v0 program-folyam.
 *
 * Réteg-felépítés (alulról):
 *   PHY     : ESP UART (half-duplex, single-wire, open-drain) a Kconfig-lábon.
 *   Link    : BREAK/SYNC + UPDI utasítások (LDS/STS/LDCS/STCS/KEY/REPEAT/ST/LD).
 *   NVM     : tinyAVR/megaAVR 0/1/2 (NVMCTRL v0) page-buffer + ERWP folyam.
 *   Orchestr: enable -> NVMPROG key -> reset prog-módba -> page program -> verify.
 *
 * FONTOS: HW-n MÉG NEM IGAZOLT. A link-réteg a dokumentált UPDI ABI szerint,
 * az NVM-folyam az NVMCTRL v0 (tinyAVR/megaAVR 0/1/2) családra készült. Az
 * AVR Dx/Ex (NVMCTRL v2+) más címeket/parancsokat használ — későbbi bővítés.
 * A signature-tábla értékeit HW-n ellenőrizni kell.
 *
 * Half-duplex echo: single-wire vonalon minden elküldött bájt visszhangzik a
 * saját RX-ünkre. Ezért minden küldés után a visszhang-bájtokat le kell üríteni,
 * és csak utána jön a cél válasza (ACK/adat).
 */
#include "avr_updi.h"

#include <string.h>
#include <stdlib.h>

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage_src.h"
#include "avr_image.h"

static const char *TAG = "avr_updi";

/* ============================ PHY paraméterek ============================ */
#define UPDI_UART        UART_NUM_2          /* UART0=konzol, UART1=target_serial */
#define UPDI_PIN         CONFIG_AVR_UPDI_GPIO
#define UPDI_BAUD        CONFIG_AVR_UPDI_BAUD
#define UPDI_RXBUF       512
#define UPDI_TXBUF       0                   /* blokkoló write (nincs TX-ring) */
#define UPDI_TIMEOUT_MS  60                  /* egy bájt válaszra várás */

static bool s_inited = false;

/* ============================ UPDI link konstansok ====================== */
#define UPDI_SYNC        0x55
#define UPDI_ACK         0x40

/* Utasítás-opkódok (SYNC után). a=address-size (0=8,1=16,2=24b), d=data-size. */
#define UPDI_LDS(a,d)    (0x00 | ((a)<<2) | (d))
#define UPDI_STS(a,d)    (0x40 | ((a)<<2) | (d))
#define UPDI_LDCS(cs)    (0x80 | ((cs) & 0x0F))
#define UPDI_STCS(cs)    (0xC0 | ((cs) & 0x0F))
#define UPDI_KEY_64      0xE0                /* KEY, 64-bites (8 bájt) kulcs */

/* 24-bites cím, 8-bites adat (egységesen ezt használjuk a map eléréséhez). */
#define LDS24  UPDI_LDS(2,0)
#define STS24  UPDI_STS(2,0)

/* Control/Status (CS) regiszterek. */
#define CS_STATUSA        0x00
#define CS_STATUSB        0x01
#define CS_CTRLA          0x02
#define CS_CTRLB          0x03
#define CS_ASI_KEY_STATUS 0x07
#define CS_ASI_RESET_REQ  0x08
#define CS_ASI_CTRLA      0x09
#define CS_ASI_SYS_CTRLA  0x0A
#define CS_ASI_SYS_STATUS 0x0B

#define CTRLA_GTVAL_2     0x06              /* guard time = 2 ciklus (leggyorsabb) */
#define RESET_REQ_APPLY   0x59
#define RESET_REQ_CLEAR   0x00

#define KEYSTAT_NVMPROG   0x10             /* ASI_KEY_STATUS: NVMPROG kulcs aktív */
#define SYSSTAT_NVMPROG   0x08             /* ASI_SYS_STATUS: prog-módban */
#define SYSSTAT_LOCKSTAT  0x01             /* ASI_SYS_STATUS: chip lockolt */

/* KEY-stringek (8 bájt, a vonalra FORDÍTVA küldve). */
static const char KEY_NVMPROG[8] = { 'N','V','M','P','r','o','g',' ' };

/* ============================ NVMCTRL v0 (tinyAVR/megaAVR 0/1/2) ========= */
#define NVM_BASE          0x1000
#define NVM_CTRLA         (NVM_BASE + 0x00)
#define NVM_STATUS        (NVM_BASE + 0x02)
#define NVM_STATUS_FBUSY  0x01
#define NVM_STATUS_WRERR  0x04

#define NVM_CMD_NOP       0x00
#define NVM_CMD_WP        0x01            /* write page */
#define NVM_CMD_ER        0x02            /* erase page */
#define NVM_CMD_ERWP      0x03            /* erase + write page */
#define NVM_CMD_PBC       0x04            /* page buffer clear */
#define NVM_CMD_CHER      0x05            /* chip erase */

#define SIGROW_ADDR       0x1100          /* signature row a UPDI data-map-ben */

/* ============================ Cél-tábla ================================= */
typedef struct {
    uint8_t  sig[3];
    const char *name;
    uint32_t flash_size;   /* bájt */
    uint16_t page_size;    /* bájt */
    uint32_t flash_base;   /* a flash leképzése a UPDI data-map-ben */
} updi_known_t;

/* MEGJEGYZÉS: a signature-értékeket HW-n ellenőrizni kell. A flash_base
 * tinyAVR-en 0x8000, megaAVR0-n 0x4000. */
static const updi_known_t UPDI_TABLE[] = {
    { { 0x1E, 0x92, 0x23 }, "ATtiny412",  4096,   64, 0x8000 },
    { { 0x1E, 0x92, 0x22 }, "ATtiny414",  4096,   64, 0x8000 },
    { { 0x1E, 0x93, 0x22 }, "ATtiny814",  8192,   64, 0x8000 },
    { { 0x1E, 0x93, 0x21 }, "ATtiny816",  8192,   64, 0x8000 },
    { { 0x1E, 0x94, 0x22 }, "ATtiny1614", 16384,  64, 0x8000 },
    { { 0x1E, 0x94, 0x21 }, "ATtiny1616", 16384,  64, 0x8000 },
    { { 0x1E, 0x95, 0x21 }, "ATtiny3216", 32768, 128, 0x8000 },
    { { 0x1E, 0x95, 0x22 }, "ATtiny3217", 32768, 128, 0x8000 },
    { { 0x1E, 0x96, 0x50 }, "ATmega4808", 49152, 128, 0x4000 },
    { { 0x1E, 0x96, 0x51 }, "ATmega4809", 49152, 128, 0x4000 },
};

static const updi_known_t *updi_lookup(const uint8_t sig[3])
{
    for (size_t i = 0; i < sizeof(UPDI_TABLE) / sizeof(UPDI_TABLE[0]); ++i) {
        if (memcmp(UPDI_TABLE[i].sig, sig, 3) == 0) return &UPDI_TABLE[i];
    }
    return NULL;
}

/* ============================ PHY (UART single-wire) ==================== */

/* A vonal felhozása a kívánt baud-on. A TX és RX UGYANARRA a lábra megy
 * (open-drain + pullup), így half-duplex single-wire. */
static esp_err_t updi_uart_setup(int baud)
{
    uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_EVEN,      /* UPDI = 8E2 */
        .stop_bits = UART_STOP_BITS_2,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(UPDI_UART, &cfg);
    if (err != ESP_OK) return err;
    err = uart_set_pin(UPDI_UART, UPDI_PIN, UPDI_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    /* Open-drain + felhúzás: a cél is tudja a vonalat alacsonyba húzni
     * (wired-AND), idle = magas. */
    gpio_set_direction(UPDI_PIN, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_pull_mode(UPDI_PIN, GPIO_PULLUP_ONLY);
    return ESP_OK;
}

/* n bájt küldése + a visszhang (echo) leürítése. A single-wire vonalon a
 * saját TX visszajön az RX-re; pontosan n bájtot olvasunk vissza és eldobunk. */
static esp_err_t updi_send(const uint8_t *buf, size_t n)
{
    int w = uart_write_bytes(UPDI_UART, (const char *)buf, n);
    if (w != (int)n) return ESP_FAIL;
    uart_wait_tx_done(UPDI_UART, pdMS_TO_TICKS(100));
    /* echo-leürítés */
    size_t got = 0;
    uint8_t tmp;
    while (got < n) {
        int r = uart_read_bytes(UPDI_UART, &tmp, 1, pdMS_TO_TICKS(UPDI_TIMEOUT_MS));
        if (r != 1) { ESP_LOGW(TAG, "echo timeout (%u/%u)", (unsigned)got, (unsigned)n); return ESP_ERR_TIMEOUT; }
        got++;
    }
    return ESP_OK;
}

/* n válasz-bájt olvasása (echo UTÁN). */
static esp_err_t updi_recv(uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        int r = uart_read_bytes(UPDI_UART, buf + got, n - got, pdMS_TO_TICKS(UPDI_TIMEOUT_MS));
        if (r <= 0) { ESP_LOGW(TAG, "recv timeout (%u/%u)", (unsigned)got, (unsigned)n); return ESP_ERR_TIMEOUT; }
        got += r;
    }
    return ESP_OK;
}

/* BREAK küldése: ideiglenesen alacsony baud -> egy 0x00 bájt hosszú low-impulzus
 * (>12 bit), amit a UPDI bármilyen állapotból detektál és idle-re áll. */
static void updi_break(void)
{
    updi_uart_setup(4800);                /* 0x00 @ 4800 ~ 1,9 ms low */
    uint8_t z = 0x00;
    uart_write_bytes(UPDI_UART, (const char *)&z, 1);
    uart_wait_tx_done(UPDI_UART, pdMS_TO_TICKS(50));
    uart_flush_input(UPDI_UART);
    updi_uart_setup(UPDI_BAUD);           /* vissza a munkasebességre */
}

/* ============================ Link-réteg utasítások ==================== */

/* STCS: control/status regiszter írása. */
static esp_err_t updi_stcs(uint8_t cs, uint8_t val)
{
    uint8_t f[3] = { UPDI_SYNC, UPDI_STCS(cs), val };
    return updi_send(f, sizeof(f));
}

/* LDCS: control/status regiszter olvasása. */
static esp_err_t updi_ldcs(uint8_t cs, uint8_t *out)
{
    uint8_t f[2] = { UPDI_SYNC, UPDI_LDCS(cs) };
    esp_err_t err = updi_send(f, sizeof(f));
    if (err != ESP_OK) return err;
    return updi_recv(out, 1);
}

/* STS (24-bites cím, 1 adatbájt): cím -> ACK -> adat -> ACK. */
static esp_err_t updi_sts8(uint32_t addr, uint8_t data)
{
    uint8_t f[5] = { UPDI_SYNC, STS24,
                     (uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16) };
    esp_err_t err = updi_send(f, sizeof(f));
    if (err != ESP_OK) return err;
    uint8_t ack;
    if ((err = updi_recv(&ack, 1)) != ESP_OK) return err;
    if (ack != UPDI_ACK) { ESP_LOGW(TAG, "STS cim-ACK=%02X", ack); return ESP_FAIL; }
    if ((err = updi_send(&data, 1)) != ESP_OK) return err;
    if ((err = updi_recv(&ack, 1)) != ESP_OK) return err;
    if (ack != UPDI_ACK) { ESP_LOGW(TAG, "STS adat-ACK=%02X", ack); return ESP_FAIL; }
    return ESP_OK;
}

/* LDS (24-bites cím, 1 adatbájt): cím -> adat. */
static esp_err_t updi_lds8(uint32_t addr, uint8_t *out)
{
    uint8_t f[5] = { UPDI_SYNC, LDS24,
                     (uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16) };
    esp_err_t err = updi_send(f, sizeof(f));
    if (err != ESP_OK) return err;
    return updi_recv(out, 1);
}

/* KEY: 64-bites kulcs betöltése (a 8 bájtot FORDÍTOTT sorrendben küldjük). */
static esp_err_t updi_key(const char key[8])
{
    uint8_t f[2] = { UPDI_SYNC, UPDI_KEY_64 };
    esp_err_t err = updi_send(f, sizeof(f));
    if (err != ESP_OK) return err;
    uint8_t rev[8];
    for (int i = 0; i < 8; ++i) rev[i] = (uint8_t)key[7 - i];
    return updi_send(rev, 8);
}

/* NVMCTRL STATUS FBUSY poll. */
static esp_err_t updi_nvm_wait(int timeout_ms)
{
    for (int t = 0; t < timeout_ms; ++t) {
        uint8_t st;
        if (updi_lds8(NVM_STATUS, &st) == ESP_OK) {
            if (st & NVM_STATUS_WRERR) { ESP_LOGE(TAG, "NVM WRERROR"); return ESP_FAIL; }
            if ((st & NVM_STATUS_FBUSY) == 0) return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGE(TAG, "NVM FBUSY timeout");
    return ESP_ERR_TIMEOUT;
}

/* ============================ Enable / belépés ========================= */

/* UPDI engedélyezése: double BREAK + guard-time beállítás + STATUSA olvasás
 * (a UPDI revízió jele, hogy él a link). */
static esp_err_t updi_enable(void)
{
    updi_break();
    updi_break();
    /* guard time leszorítása a sebességért */
    if (updi_stcs(CS_CTRLA, CTRLA_GTVAL_2) != ESP_OK)
        ESP_LOGW(TAG, "CTRLA guard-time STCS nem ACK-zott");

    uint8_t sa = 0;
    esp_err_t err = updi_ldcs(CS_STATUSA, &sa);
    if (err != ESP_OK) { ESP_LOGE(TAG, "UPDI nem valaszol (STATUSA)"); return err; }
    ESP_LOGI(TAG, "UPDI elve, STATUSA=%02X (UPDIREV=%u)", sa, (sa >> 4) & 0x0F);
    return ESP_OK;
}

/* Programozó (NVM) módba lépés: NVMPROG kulcs -> reset -> ASI_SYS_STATUS NVMPROG. */
static esp_err_t updi_enter_nvm(void)
{
    esp_err_t err = updi_enable();
    if (err != ESP_OK) return err;

    uint8_t sys = 0;
    if (updi_ldcs(CS_ASI_SYS_STATUS, &sys) == ESP_OK && (sys & SYSSTAT_LOCKSTAT)) {
        ESP_LOGE(TAG, "A chip LOCKolt (SYS_STATUS=%02X) — chip erase kulcs kell", sys);
        return ESP_ERR_INVALID_STATE;
    }

    if ((err = updi_key(KEY_NVMPROG)) != ESP_OK) { ESP_LOGE(TAG, "NVMPROG KEY hiba"); return err; }

    uint8_t ks = 0;
    updi_ldcs(CS_ASI_KEY_STATUS, &ks);
    if ((ks & KEYSTAT_NVMPROG) == 0) {
        ESP_LOGE(TAG, "NVMPROG kulcs nem aktiv (KEY_STATUS=%02X)", ks);
        return ESP_FAIL;
    }

    /* Reset-pulzus, hogy a kulcs érvénybe lépjen. */
    updi_stcs(CS_ASI_RESET_REQ, RESET_REQ_APPLY);
    updi_stcs(CS_ASI_RESET_REQ, RESET_REQ_CLEAR);

    /* Várjuk a NVMPROG-állapotot. */
    for (int t = 0; t < 50; ++t) {
        if (updi_ldcs(CS_ASI_SYS_STATUS, &sys) == ESP_OK && (sys & SYSSTAT_NVMPROG)) {
            ESP_LOGI(TAG, "NVM prog-mod aktiv (SYS_STATUS=%02X)", sys);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGE(TAG, "Nem leptunk NVM prog-modba (SYS_STATUS=%02X)", sys);
    return ESP_FAIL;
}

/* Kilépés: reset -> az alkalmazás fut, a UPDI elenged. */
static void updi_leave(void)
{
    updi_stcs(CS_ASI_RESET_REQ, RESET_REQ_APPLY);
    updi_stcs(CS_ASI_RESET_REQ, RESET_REQ_CLEAR);
    ESP_LOGI(TAG, "UPDI leave: cel elengedve");
}

/* Signature 3 bájt a SIGROW-ból. */
static esp_err_t updi_read_signature(uint8_t out[3])
{
    for (int i = 0; i < 3; ++i) {
        esp_err_t err = updi_lds8(SIGROW_ADDR + i, &out[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

/* ============================ Public: init ============================= */
esp_err_t avr_updi_init(void)
{
    if (s_inited) return ESP_OK;
    esp_err_t err = uart_driver_install(UPDI_UART, UPDI_RXBUF, UPDI_TXBUF, 0, NULL, 0);
    if (err != ESP_OK) { ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err)); return err; }
    err = updi_uart_setup(UPDI_BAUD);
    if (err != ESP_OK) { ESP_LOGE(TAG, "uart setup: %s", esp_err_to_name(err)); return err; }
    s_inited = true;
    ESP_LOGI(TAG, "AVR UPDI init: GPIO%d @ %d baud (single-wire 8E2)", UPDI_PIN, UPDI_BAUD);
    return ESP_OK;
}

/* ============================ Public: detect ========================== */
esp_err_t avr_updi_detect(avr_updi_dev_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_inited) { esp_err_t e = avr_updi_init(); if (e != ESP_OK) return e; }
    memset(out, 0, sizeof(*out));

    esp_err_t err = updi_enable();
    if (err != ESP_OK) return err;

    err = updi_read_signature(out->sig);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Signature olvasas hiba"); updi_leave(); return err; }
    ESP_LOGI(TAG, "Signature: %02X %02X %02X", out->sig[0], out->sig[1], out->sig[2]);

    const updi_known_t *k = updi_lookup(out->sig);
    if (k) {
        out->known = true; out->name = k->name;
        out->flash_size = k->flash_size; out->page_size = k->page_size;
        ESP_LOGI(TAG, "Eszkoz: %s (flash %u B, lap %u B)",
                 k->name, (unsigned)k->flash_size, (unsigned)k->page_size);
    } else {
        ESP_LOGW(TAG, "Ismeretlen UPDI signature, nincs a tablaban");
    }

    updi_leave();
    return ESP_OK;
}

/* ============================ Public: flash file ====================== */
esp_err_t avr_updi_flash_file(const char *path, avr_updi_progress_cb cb, void *ctx)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (!s_inited) { esp_err_t e = avr_updi_init(); if (e != ESP_OK) return e; }

    /* 1) Forrásfájl beolvasása az aktív forrásból (mi free-zünk). */
    void *raw = NULL;
    size_t raw_len = 0;
    esp_err_t err = storage_src_read_all(path, &raw, &raw_len);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Fajl olvasas hiba: %s (%s)", path, esp_err_to_name(err)); return err; }
    ESP_LOGI(TAG, "Forras: %s (%u B)", path, (unsigned)raw_len);

    uint8_t *img = NULL;
    esp_err_t ret = ESP_FAIL;

    /* 2) NVM prog-módba lépés + signature. */
    err = updi_enter_nvm();
    if (err != ESP_OK) { free(raw); return err; }

    uint8_t sig[3] = {0};
    updi_read_signature(sig);
    ESP_LOGI(TAG, "Signature: %02X %02X %02X", sig[0], sig[1], sig[2]);
    const updi_known_t *k = updi_lookup(sig);
    if (!k) {
        ESP_LOGE(TAG, "Ismeretlen UPDI signature — flashelest nem kockaztatok");
        ret = ESP_ERR_NOT_FOUND; goto out;
    }
    uint32_t flash_size = k->flash_size;
    uint16_t page_size  = k->page_size;
    uint32_t flash_base = k->flash_base;
    ESP_LOGI(TAG, "Eszkoz: %s (flash %u B, lap %u B, base 0x%lX)",
             k->name, (unsigned)flash_size, (unsigned)page_size, (unsigned long)flash_base);

    /* 3) Flash-kép puffer (0xFF = törölt). */
    img = malloc(flash_size);
    if (!img) { ret = ESP_ERR_NO_MEM; goto out; }
    memset(img, 0xFF, flash_size);

    size_t img_len = 0;
    err = avr_image_parse(path, raw, raw_len, img, flash_size, &img_len);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Kep betoltes hiba (%s)", esp_err_to_name(err)); ret = err; goto out; }
    if (img_len == 0) { ESP_LOGW(TAG, "Ures kep, nincs mit irni"); ret = ESP_OK; goto out; }

    /* Lapra kerekítés (az utolsó lap maradéka 0xFF). */
    size_t prog_len = ((img_len + page_size - 1) / page_size) * page_size;
    if (prog_len > flash_size) prog_len = flash_size;
    size_t total_pages = prog_len / page_size;

    /* 4) Lap-programozás: page buffer clear -> bájtok a mapelt flash-be -> ERWP. */
    if (cb) cb("Iras", 0, ctx);
    for (size_t p = 0; p < total_pages; ++p) {
        uint32_t page_addr = flash_base + (uint32_t)(p * page_size);

        if ((err = updi_sts8(NVM_CTRLA, NVM_CMD_PBC)) != ESP_OK) { ret = err; goto out; }
        if ((err = updi_nvm_wait(50)) != ESP_OK) { ret = err; goto out; }

        for (uint16_t b = 0; b < page_size; ++b) {
            if ((err = updi_sts8(page_addr + b, img[p * page_size + b])) != ESP_OK) {
                ESP_LOGE(TAG, "Page-buffer iras hiba (lap %u, ofs %u)", (unsigned)p, b);
                ret = err; goto out;
            }
        }
        if ((err = updi_sts8(NVM_CTRLA, NVM_CMD_ERWP)) != ESP_OK) { ret = err; goto out; }
        if ((err = updi_nvm_wait(50)) != ESP_OK) { ret = err; goto out; }

        int pct = (int)(((p + 1) * 100) / total_pages);
        if (cb) cb("Iras", pct, ctx);
    }
    ESP_LOGI(TAG, "Iras kesz: %u B", (unsigned)prog_len);

    /* 5) Verify: visszaolvasás + összevetés. */
    for (size_t i = 0; i < prog_len; ++i) {
        uint8_t got;
        if ((err = updi_lds8(flash_base + i, &got)) != ESP_OK) { ret = err; goto out; }
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
    ESP_LOGI(TAG, "Verify OK: %u B egyezik", (unsigned)prog_len);
    ret = ESP_OK;

out:
    updi_leave();
    if (img) free(img);
    free(raw);
    if (ret == ESP_OK) ESP_LOGI(TAG, "UPDI flashelés kész: %s", path);
    return ret;
}
