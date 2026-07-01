/* AVR UPDI programozó — UART single-wire (8E2) + NVMCTRL v0/v2 program-folyam.
 *
 * Réteg-felépítés (alulról):
 *   PHY     : ESP UART (half-duplex, single-wire, open-drain) a Kconfig-lábon.
 *   Link    : BREAK/SYNC + UPDI utasítások (LDS/STS/LDCS/STCS/KEY/ST-ptr).
 *   NVM     : NVMCTRL v0 (tinyAVR/megaAVR 0/1/2) ÉS v2 (AVR Dx) page-program.
 *   Orchestr: enable -> NVMPROG key -> reset prog-módba -> chip erase -> page
 *             program -> verify.
 *
 * A link-réteg konstansai és a program-szekvenciák a Microchip pymcuprog
 * (serialupdi/constants.py, link.py, nvmp0.py, nvmp2.py) forrásához igazítva.
 * HW-n MÉG NEM IGAZOLT, de a protokoll a referencia-eszközzel egyeztetve van.
 * Lásd: reference/AVR_UPDI.md.
 *
 * Half-duplex echo: single-wire vonalon minden elküldött bájt visszhangzik a
 * saját RX-ünkre; küldés után a visszhangot leürítjük, utána jön az ACK/adat.
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

/* Base-opkódok + minősítő bitek (pymcuprog serialupdi/constants.py). */
#define OP_LDS   0x00
#define OP_STS   0x40
#define OP_LD    0x20
#define OP_ST    0x60
#define OP_LDCS  0x80
#define OP_STCS  0xC0
#define OP_REPEAT 0xA0
#define OP_KEY   0xE0
/* pointer-módok (LD/ST): */
#define PTR_PTR      0x00
#define PTR_INC      0x04
#define PTR_ADDRESS  0x08
/* cím-/adatméret bitek: */
#define ADDR_8   0x00
#define ADDR_16  0x04
#define ADDR_24  0x08
#define DATA_8   0x00
#define DATA_16  0x01

/* Származtatott opkódok. */
#define LDS24       (OP_LDS | ADDR_24 | DATA_8)   /* 0x08 */
#define STS24       (OP_STS | ADDR_24 | DATA_8)   /* 0x48 */
#define ST_PTR16    (OP_ST | PTR_ADDRESS | DATA_16) /* 0x69: ptr set, 2 cím */
#define ST_PTR24    (OP_ST | PTR_ADDRESS | 0x02)  /* 0x6A: ptr set, 3 cím (DATA_24) */
#define ST_INC16    (OP_ST | PTR_INC | DATA_16)   /* 0x65: *(ptr++) word */

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

/* CTRLA: IBDLY=bit7, RSD=bit3, GTVAL=bit[2:0]. pymcuprog normál üzemi értéke
 * IBDLY (0x80, guard-time default). */
#define CTRLA_IBDLY       0x80
#define RESET_REQ_APPLY   0x59
#define RESET_REQ_CLEAR   0x00

#define KEYSTAT_NVMPROG   0x10             /* ASI_KEY_STATUS: NVMPROG (bit4) */
#define SYSSTAT_NVMPROG   0x08             /* ASI_SYS_STATUS: prog-módban (bit3) */
#define SYSSTAT_LOCKSTAT  0x01             /* ASI_SYS_STATUS: chip lockolt (bit0) */

/* KEY-string (8 bájt, a vonalra FORDÍTVA — pymcuprog link.key() reversed()). */
static const char KEY_NVMPROG[8] = { 'N','V','M','P','r','o','g',' ' };

/* ============================ NVMCTRL (v0 és v2) ======================== */
#define NVM_BASE          0x1000            /* minden UPDI-parton */
#define NVM_CTRLA         (NVM_BASE + 0x00)
#define NVM_STATUS        (NVM_BASE + 0x02)
#define NVM_STATUS_FBUSY  0x01
#define NVM_STATUS_EEBUSY 0x02
#define NVM_ERRMASK_V0    0x04             /* WRERROR (bit2) */
#define NVM_ERRMASK_V2    0x30             /* ERROR mező (bit5:4) */

/* NVMCTRL v0 parancsok (tinyAVR/megaAVR 0/1/2). */
#define V0_CMD_NOP        0x00
#define V0_CMD_WP         0x01             /* write page (buffer -> flash) */
#define V0_CMD_ER         0x02             /* erase page */
#define V0_CMD_ERWP       0x03             /* erase+write page */
#define V0_CMD_PBC        0x04             /* page buffer clear */
#define V0_CMD_CHER       0x05             /* chip erase */

/* NVMCTRL v2 parancsok (AVR Dx). */
#define V2_CMD_NOCMD      0x00
#define V2_CMD_FLASH_WR   0x02             /* flash page write */
#define V2_CMD_FLASH_PER  0x08             /* flash page erase */
#define V2_CMD_CHIP_ERASE 0x20

#define SIGROW_ADDR       0x1100          /* signature row (tiny/mega0; Dx-en ellenőrizendő) */

/* ============================ Cél-tábla ================================= */
typedef struct {
    uint8_t  sig[3];
    const char *name;
    uint32_t flash_size;   /* bájt */
    uint16_t page_size;    /* bájt */
    uint32_t flash_base;   /* flash leképzése a UPDI data-map-ben */
    uint8_t  nvm_ver;      /* 0 = NVMCTRL v0, 2 = v2 (AVR Dx) */
    bool     addr24;       /* true: 24-bites ST-pointer (AVR Dx) */
} updi_known_t;

/* pymcuprog deviceinfo-hoz igazítva (signature/flash/lap/base/verzió). */
static const updi_known_t UPDI_TABLE[] = {
    /* --- NVMCTRL v0: tinyAVR (flash @ 0x8000) --- */
    { { 0x1E, 0x92, 0x23 }, "ATtiny412",  0x1000,  64, 0x8000, 0, false },
    { { 0x1E, 0x92, 0x22 }, "ATtiny414",  0x1000,  64, 0x8000, 0, false },
    { { 0x1E, 0x93, 0x22 }, "ATtiny814",  0x2000,  64, 0x8000, 0, false },
    { { 0x1E, 0x93, 0x21 }, "ATtiny816",  0x2000,  64, 0x8000, 0, false },
    { { 0x1E, 0x94, 0x22 }, "ATtiny1614", 0x4000,  64, 0x8000, 0, false },
    { { 0x1E, 0x94, 0x21 }, "ATtiny1616", 0x4000,  64, 0x8000, 0, false },
    { { 0x1E, 0x95, 0x21 }, "ATtiny3216", 0x8000, 128, 0x8000, 0, false },
    { { 0x1E, 0x95, 0x22 }, "ATtiny3217", 0x8000, 128, 0x8000, 0, false },
    /* --- NVMCTRL v0: megaAVR 0 (flash @ 0x4000) --- */
    { { 0x1E, 0x96, 0x50 }, "ATmega4808", 0xC000, 128, 0x4000, 0, false },
    { { 0x1E, 0x96, 0x51 }, "ATmega4809", 0xC000, 128, 0x4000, 0, false },
    /* --- NVMCTRL v2: AVR Dx (24-bit, flash @ 0x800000, 512 B lap) --- */
    { { 0x1E, 0x97, 0x08 }, "AVR128DA48", 0x20000, 512, 0x800000, 2, true },
    { { 0x1E, 0x96, 0x1A }, "AVR64DD32",  0x10000, 512, 0x800000, 2, true },
    { { 0x1E, 0x97, 0x0C }, "AVR128DB48", 0x20000, 512, 0x800000, 2, true },
};

static const updi_known_t *updi_lookup(const uint8_t sig[3])
{
    for (size_t i = 0; i < sizeof(UPDI_TABLE) / sizeof(UPDI_TABLE[0]); ++i) {
        if (memcmp(UPDI_TABLE[i].sig, sig, 3) == 0) return &UPDI_TABLE[i];
    }
    return NULL;
}

/* ============================ PHY (UART single-wire) ==================== */

/* A vonal felhozása a kívánt baud-on. TX és RX ugyanarra a lábra (open-drain +
 * pullup) → half-duplex single-wire. */
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
    gpio_set_direction(UPDI_PIN, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_pull_mode(UPDI_PIN, GPIO_PULLUP_ONLY);
    return ESP_OK;
}

/* n bájt küldése + a visszhang (echo) leürítése. */
static esp_err_t updi_send(const uint8_t *buf, size_t n)
{
    int w = uart_write_bytes(UPDI_UART, (const char *)buf, n);
    if (w != (int)n) return ESP_FAIL;
    uart_wait_tx_done(UPDI_UART, pdMS_TO_TICKS(100));
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

/* Egy bájt küldése + ACK (0x40) várása. */
static esp_err_t updi_send_ack(const uint8_t *buf, size_t n)
{
    esp_err_t err = updi_send(buf, n);
    if (err != ESP_OK) return err;
    uint8_t ack;
    if ((err = updi_recv(&ack, 1)) != ESP_OK) return err;
    if (ack != UPDI_ACK) { ESP_LOGW(TAG, "vart ACK, kapott %02X", ack); return ESP_FAIL; }
    return ESP_OK;
}

/* BREAK: alacsony baud -> egy 0x00 hosszú low-impulzus (>12 bit). */
static void updi_break(void)
{
    updi_uart_setup(4800);
    uint8_t z = 0x00;
    uart_write_bytes(UPDI_UART, (const char *)&z, 1);
    uart_wait_tx_done(UPDI_UART, pdMS_TO_TICKS(50));
    uart_flush_input(UPDI_UART);
    updi_uart_setup(UPDI_BAUD);
}

/* ============================ Link-réteg utasítások ==================== */

static esp_err_t updi_stcs(uint8_t cs, uint8_t val)
{
    uint8_t f[3] = { UPDI_SYNC, (uint8_t)(OP_STCS | (cs & 0x0F)), val };
    return updi_send(f, sizeof(f));
}

static esp_err_t updi_ldcs(uint8_t cs, uint8_t *out)
{
    uint8_t f[2] = { UPDI_SYNC, (uint8_t)(OP_LDCS | (cs & 0x0F)) };
    esp_err_t err = updi_send(f, sizeof(f));
    if (err != ESP_OK) return err;
    return updi_recv(out, 1);
}

/* STS (24-bites cím, 1 adatbájt): cím -> ACK -> adat -> ACK. */
static esp_err_t updi_sts8(uint32_t addr, uint8_t data)
{
    uint8_t f[5] = { UPDI_SYNC, STS24,
                     (uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16) };
    esp_err_t err = updi_send_ack(f, sizeof(f));      /* cím-ACK */
    if (err != ESP_OK) return err;
    return updi_send_ack(&data, 1);                   /* adat-ACK */
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

/* ST pointer beállítása (16- vagy 24-bites cím), ACK. */
static esp_err_t updi_st_ptr(uint32_t addr, bool addr24)
{
    if (addr24) {
        uint8_t f[5] = { UPDI_SYNC, ST_PTR24,
                         (uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16) };
        return updi_send_ack(f, sizeof(f));
    }
    uint8_t f[4] = { UPDI_SYNC, ST_PTR16, (uint8_t)addr, (uint8_t)(addr >> 8) };
    return updi_send_ack(f, sizeof(f));
}

/* ST *(ptr++) = word (lo,hi), ACK. */
static esp_err_t updi_st_inc_word(uint8_t lo, uint8_t hi)
{
    uint8_t f[4] = { UPDI_SYNC, ST_INC16, lo, hi };
    return updi_send_ack(f, sizeof(f));
}

/* KEY: 64-bites kulcs (8 bájt FORDÍTOTT sorrendben). */
static esp_err_t updi_key(const char key[8])
{
    uint8_t f[2] = { UPDI_SYNC, (uint8_t)(OP_KEY | 0x00) };  /* 64-bit */
    esp_err_t err = updi_send(f, sizeof(f));
    if (err != ESP_OK) return err;
    uint8_t rev[8];
    for (int i = 0; i < 8; ++i) rev[i] = (uint8_t)key[7 - i];
    return updi_send(rev, 8);
}

/* NVMCTRL STATUS BUSY poll (err_mask verzióspecifikus). */
static esp_err_t updi_nvm_wait(int timeout_ms, uint8_t err_mask)
{
    for (int t = 0; t < timeout_ms; ++t) {
        uint8_t st;
        if (updi_lds8(NVM_STATUS, &st) == ESP_OK) {
            if (st & err_mask) { ESP_LOGE(TAG, "NVM WRERROR (STATUS=%02X)", st); return ESP_FAIL; }
            if ((st & (NVM_STATUS_FBUSY | NVM_STATUS_EEBUSY)) == 0) return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGE(TAG, "NVM BUSY timeout");
    return ESP_ERR_TIMEOUT;
}

/* Egy lap page-bufferének feltöltése: ST-pointer + word-írások (ACK-olt). */
static esp_err_t updi_fill_page(uint32_t page_addr, const uint8_t *buf,
                                uint16_t page_size, bool addr24)
{
    esp_err_t err = updi_st_ptr(page_addr, addr24);
    if (err != ESP_OK) return err;
    for (uint16_t w = 0; w < page_size / 2; ++w) {
        if ((err = updi_st_inc_word(buf[2 * w], buf[2 * w + 1])) != ESP_OK) return err;
    }
    return ESP_OK;
}

/* ============================ Enable / belépés ========================= */

static esp_err_t updi_enable(void)
{
    updi_break();
    updi_break();
    if (updi_stcs(CS_CTRLA, CTRLA_IBDLY) != ESP_OK)
        ESP_LOGW(TAG, "CTRLA STCS nem ACK-zott");

    uint8_t sa = 0;
    esp_err_t err = updi_ldcs(CS_STATUSA, &sa);
    if (err != ESP_OK) { ESP_LOGE(TAG, "UPDI nem valaszol (STATUSA)"); return err; }
    ESP_LOGI(TAG, "UPDI elve, STATUSA=%02X (UPDIREV=%u)", sa, (sa >> 4) & 0x0F);
    return ESP_OK;
}

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

    updi_stcs(CS_ASI_RESET_REQ, RESET_REQ_APPLY);
    updi_stcs(CS_ASI_RESET_REQ, RESET_REQ_CLEAR);

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

static void updi_leave(void)
{
    updi_stcs(CS_ASI_RESET_REQ, RESET_REQ_APPLY);
    updi_stcs(CS_ASI_RESET_REQ, RESET_REQ_CLEAR);
    ESP_LOGI(TAG, "UPDI leave: cel elengedve");
}

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
        ESP_LOGI(TAG, "Eszkoz: %s (flash %u B, lap %u B, NVMv%u)",
                 k->name, (unsigned)k->flash_size, (unsigned)k->page_size, k->nvm_ver);
    } else {
        ESP_LOGW(TAG, "Ismeretlen UPDI signature, nincs a tablaban");
    }

    updi_leave();
    return ESP_OK;
}

/* ============================ Program-folyam (v0/v2) ================== */

/* Egy lap programozása a verziónak megfelelő szekvenciával. */
static esp_err_t updi_program_page(const updi_known_t *k, uint32_t page_addr,
                                   const uint8_t *page)
{
    esp_err_t err;
    if (k->nvm_ver == 2) {
        /* v2 (AVR Dx): FLASH_WRITE -> fill (word) -> wait -> NOCMD. */
        if ((err = updi_nvm_wait(50, NVM_ERRMASK_V2)) != ESP_OK) return err;
        if ((err = updi_sts8(NVM_CTRLA, V2_CMD_FLASH_WR)) != ESP_OK) return err;
        if ((err = updi_fill_page(page_addr, page, k->page_size, k->addr24)) != ESP_OK) return err;
        if ((err = updi_nvm_wait(50, NVM_ERRMASK_V2)) != ESP_OK) return err;
        return updi_sts8(NVM_CTRLA, V2_CMD_NOCMD);
    }
    /* v0 (tiny/mega0): PBC -> fill -> WP -> wait. (chip erase előre megvolt.) */
    if ((err = updi_nvm_wait(50, NVM_ERRMASK_V0)) != ESP_OK) return err;
    if ((err = updi_sts8(NVM_CTRLA, V0_CMD_PBC)) != ESP_OK) return err;
    if ((err = updi_nvm_wait(50, NVM_ERRMASK_V0)) != ESP_OK) return err;
    if ((err = updi_fill_page(page_addr, page, k->page_size, k->addr24)) != ESP_OK) return err;
    if ((err = updi_sts8(NVM_CTRLA, V0_CMD_WP)) != ESP_OK) return err;
    return updi_nvm_wait(50, NVM_ERRMASK_V0);
}

/* Teljes flash (app) chip erase a verziónak megfelelő paranccsal. */
static esp_err_t updi_chip_erase(const updi_known_t *k)
{
    uint8_t cmd = (k->nvm_ver == 2) ? V2_CMD_CHIP_ERASE : V0_CMD_CHER;
    uint8_t errmask = (k->nvm_ver == 2) ? NVM_ERRMASK_V2 : NVM_ERRMASK_V0;
    esp_err_t err = updi_nvm_wait(50, errmask);
    if (err != ESP_OK) return err;
    if ((err = updi_sts8(NVM_CTRLA, cmd)) != ESP_OK) return err;
    err = updi_nvm_wait(500, errmask);
    if (k->nvm_ver == 2) updi_sts8(NVM_CTRLA, V2_CMD_NOCMD);
    return err;
}

/* ============================ Public: flash file ====================== */
esp_err_t avr_updi_flash_file(const char *path, avr_updi_progress_cb cb, void *ctx)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (!s_inited) { esp_err_t e = avr_updi_init(); if (e != ESP_OK) return e; }

    void *raw = NULL;
    size_t raw_len = 0;
    esp_err_t err = storage_src_read_all(path, &raw, &raw_len);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Fajl olvasas hiba: %s (%s)", path, esp_err_to_name(err)); return err; }
    ESP_LOGI(TAG, "Forras: %s (%u B)", path, (unsigned)raw_len);

    uint8_t *img = NULL;
    esp_err_t ret = ESP_FAIL;

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
    ESP_LOGI(TAG, "Eszkoz: %s (flash %u B, lap %u B, NVMv%u, base 0x%lX)",
             k->name, (unsigned)k->flash_size, (unsigned)k->page_size,
             k->nvm_ver, (unsigned long)k->flash_base);

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
    uint8_t errmask = (k->nvm_ver == 2) ? NVM_ERRMASK_V2 : NVM_ERRMASK_V0;

    /* Chip erase (a WP/FLASH_WRITE nem töröl implicit módon). */
    if (cb) cb("Torles", 0, ctx);
    if ((err = updi_chip_erase(k)) != ESP_OK) { ret = err; goto out; }
    if (cb) cb("Torles", 100, ctx);

    /* Lap-programozás. */
    if (cb) cb("Iras", 0, ctx);
    for (size_t p = 0; p < total_pages; ++p) {
        uint32_t page_addr = k->flash_base + (uint32_t)(p * k->page_size);
        if ((err = updi_program_page(k, page_addr, &img[p * k->page_size])) != ESP_OK) {
            ESP_LOGE(TAG, "Lap-programozas hiba (lap %u)", (unsigned)p);
            ret = err; goto out;
        }
        int pct = (int)(((p + 1) * 100) / total_pages);
        if (cb) cb("Iras", pct, ctx);
    }
    ESP_LOGI(TAG, "Iras kesz: %u B", (unsigned)prog_len);
    (void)errmask;

    /* Verify: visszaolvasás + összevetés. */
    for (size_t i = 0; i < prog_len; ++i) {
        uint8_t got;
        if ((err = updi_lds8(k->flash_base + i, &got)) != ESP_OK) { ret = err; goto out; }
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
