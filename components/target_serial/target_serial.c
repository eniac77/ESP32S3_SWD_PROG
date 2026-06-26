#include "target_serial.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "target_state.h"
#include "storage_lfs.h"

static const char *TAG = "target_serial";

/* ======================================================================
 * 13.1 Transzport — UART konfiguráció
 * ====================================================================== */
#define TS_UART_NUM        UART_NUM_1
#define TS_PIN_TX          GPIO_NUM_17       /* terv 2.2: TX=GPIO17 */
#define TS_PIN_RX          GPIO_NUM_18       /* terv 2.2: RX=GPIO18 */
#define TS_DEFAULT_BAUD    115200
#define TS_RX_BUF_SZ       1024              /* UART driver RX ring buffer */
#define TS_TX_BUF_SZ       0                 /* 0 = blokkoló write, nincs TX ring */

/* ======================================================================
 * Keret-formátum:  SOF(0xAA) | LEN(2, LE = payload hossz) | CMD(1) | PAYLOAD | CRC16(2, LE)
 * A CRC16 a LEN..PAYLOAD bájtokra (a SOF nélkül, a CRC nélkül) számolódik.
 * ====================================================================== */
#define TS_SOF             0xAA
#define TS_MAX_PAYLOAD     512
#define TS_HDR_LEN         3                 /* LEN(2) + CMD(1) — a CRC-vel fedett fejléc */
#define TS_FRAME_OVERHEAD  (1 /*SOF*/ + TS_HDR_LEN + 2 /*CRC*/)
#define TS_MAX_FRAME       (TS_FRAME_OVERHEAD + TS_MAX_PAYLOAD)

/* ----------------------------------------------------------------------
 * Protokoll-adapter (terv 13.2):
 * Ezek az alkalmazás-szintű parancs-ID-k. EZT KELL a saját cél-protokollodhoz
 * igazítani (a binárisszerver-stílusú GET_STATUS/SET-param mintádra).
 * -------------------------------------------------------------------- */
#define CMD_STATUS         0x10   /* cél -> híd: periodikus státusz-frame */
#define CMD_GET_CONFIG     0x20   /* híd -> cél: konfig blokk lekérése (payload: u16 offset LE) */
#define CMD_SET_CONFIG     0x21   /* híd -> cél: konfig blokk írása (payload: u16 offset LE + adat) */
#define CMD_COMMIT         0x22   /* híd -> cél: konfig véglegesítése (mentés) */

/* A cfg le/feltöltés blokk-mérete: egy keret payloadjába férő hasznos adat
 * (2 bájt offset fejléc levonva). Ez is a cél-protokollhoz igazítandó. */
#define CMD_BLOCK_DATA     256
#define CMD_BLOCK_HDR      2      /* u16 offset, little-endian */

/* ======================================================================
 * Modul-állapot
 * ====================================================================== */
typedef struct {
    uint8_t cmd;
    uint8_t payload[TS_MAX_PAYLOAD];
    size_t  payload_len;
} ts_frame_t;

static QueueHandle_t   s_resp_q;     /* RX-task -> command-hívó: egy várt válasz-keret */
static SemaphoreHandle_t s_tx_mtx;   /* sorosítja a command()/cfg_*() hívásokat */
static TaskHandle_t    s_rx_task;
static bool            s_inited = false;

/* ======================================================================
 * CRC16-CCITT (poly 0x1021, init 0xFFFF) — helyi implementáció
 * ====================================================================== */
static uint16_t ts_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) {
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/* ======================================================================
 * Frame-build: a kimenő puffert SOF|LEN|CMD|PAYLOAD|CRC16 formára tölti.
 * Visszaadja a teljes keret hosszát, vagy 0 hiba esetén.
 * out-nak legalább TS_MAX_FRAME méretűnek kell lennie.
 * ====================================================================== */
static size_t ts_frame_build(uint8_t cmd, const uint8_t *payload, size_t payload_len,
                             uint8_t *out, size_t out_cap)
{
    if (payload_len > TS_MAX_PAYLOAD) {
        return 0;
    }
    size_t total = TS_FRAME_OVERHEAD + payload_len;
    if (out_cap < total) {
        return 0;
    }

    size_t i = 0;
    out[i++] = TS_SOF;
    /* a CRC-vel fedett rész itt kezdődik */
    size_t crc_start = i;
    out[i++] = (uint8_t)(payload_len & 0xFF);          /* LEN low  */
    out[i++] = (uint8_t)((payload_len >> 8) & 0xFF);   /* LEN high */
    out[i++] = cmd;
    if (payload_len && payload) {
        memcpy(&out[i], payload, payload_len);
        i += payload_len;
    }
    uint16_t crc = ts_crc16(&out[crc_start], TS_HDR_LEN + payload_len);
    out[i++] = (uint8_t)(crc & 0xFF);                  /* CRC low  */
    out[i++] = (uint8_t)((crc >> 8) & 0xFF);           /* CRC high */
    return i;
}

/* Egy kész keret kiküldése a UART-ra. */
static esp_err_t ts_send_frame(uint8_t cmd, const uint8_t *payload, size_t payload_len)
{
    uint8_t buf[TS_MAX_FRAME];
    size_t n = ts_frame_build(cmd, payload, payload_len, buf, sizeof(buf));
    if (n == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int w = uart_write_bytes(TS_UART_NUM, buf, n);
    if (w != (int)n) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ======================================================================
 * Státusz-frame feldolgozása (terv 13.3).
 *
 * FELTÉTELEZETT státusz-payload formátum (a cél-protokollhoz igazítandó):
 *   uptime : u32 little-endian
 *   utána  : N db float (4 bájt/float) little-endian, ahol
 *            N = (payload_len - 4) / 4, maximum TARGET_STATE_MAX_VALUES.
 * ====================================================================== */
static void ts_handle_status(const uint8_t *payload, size_t len)
{
    if (len < 4) {
        return;
    }
    uint32_t uptime;
    memcpy(&uptime, payload, 4);   /* LE — az ESP32 little-endian, közvetlen másolható */

    size_t avail = (len - 4) / 4;
    if (avail > TARGET_STATE_MAX_VALUES) {
        avail = TARGET_STATE_MAX_VALUES;
    }
    float vals[TARGET_STATE_MAX_VALUES];
    for (size_t i = 0; i < avail; i++) {
        memcpy(&vals[i], payload + 4 + i * 4, 4);
    }
    target_state_set_values(vals, (uint8_t)avail, uptime);
}

/* ======================================================================
 * RX-task: SOF-keresés, keret-beolvasás, CRC-ellenőrzés.
 *   - CMD_STATUS  -> target_state frissítés
 *   - egyéb (válasz) -> s_resp_q-ra téve a command()-nak
 * Bármely sikeres keretnél serial_link=true; tartós csendnél/hibánál false.
 * ====================================================================== */
static bool ts_read_exact(uint8_t *dst, size_t n, TickType_t to)
{
    size_t got = 0;
    while (got < n) {
        int r = uart_read_bytes(TS_UART_NUM, dst + got, n - got, to);
        if (r <= 0) {
            return false;   /* timeout vagy hiba */
        }
        got += (size_t)r;
    }
    return true;
}

static void ts_rx_task(void *arg)
{
    (void)arg;
    const TickType_t byte_to = pdMS_TO_TICKS(50);   /* kereten belüli bájt-timeout */
    bool link_up = false;

    for (;;) {
        uint8_t b;
        /* SOF keresése — 1 s blokk, hogy a tartós csendet észleljük */
        int r = uart_read_bytes(TS_UART_NUM, &b, 1, pdMS_TO_TICKS(1000));
        if (r <= 0) {
            if (link_up) {
                link_up = false;
                target_state_set_serial(false);
            }
            continue;
        }
        if (b != TS_SOF) {
            continue;   /* re-szinkronizálás: keressük tovább a SOF-ot */
        }

        /* LEN(2) + CMD(1) */
        uint8_t hdr[TS_HDR_LEN];
        if (!ts_read_exact(hdr, TS_HDR_LEN, byte_to)) {
            continue;
        }
        size_t payload_len = (size_t)hdr[0] | ((size_t)hdr[1] << 8);
        uint8_t cmd = hdr[2];
        if (payload_len > TS_MAX_PAYLOAD) {
            continue;   /* hibás hossz — eldobjuk, újra SOF-ra keresünk */
        }

        uint8_t payload[TS_MAX_PAYLOAD];
        if (payload_len && !ts_read_exact(payload, payload_len, byte_to)) {
            continue;
        }

        uint8_t crc_raw[2];
        if (!ts_read_exact(crc_raw, 2, byte_to)) {
            continue;
        }
        uint16_t crc_rx = (uint16_t)crc_raw[0] | ((uint16_t)crc_raw[1] << 8);

        /* CRC a LEN..PAYLOAD felett: a hdr-t és a payloadot egy temp pufferbe
         * fűzzük, hogy egy hívással, folytatólagosan számoljunk. */
        uint8_t tmp[TS_HDR_LEN + TS_MAX_PAYLOAD];
        memcpy(tmp, hdr, TS_HDR_LEN);
        if (payload_len) {
            memcpy(tmp + TS_HDR_LEN, payload, payload_len);
        }
        uint16_t crc_calc = ts_crc16(tmp, TS_HDR_LEN + payload_len);
        if (crc_calc != crc_rx) {
            ESP_LOGW(TAG, "CRC hiba (cmd=0x%02x len=%u)", cmd, (unsigned)payload_len);
            continue;
        }

        /* érvényes keret -> a link él */
        if (!link_up) {
            link_up = true;
            target_state_set_serial(true);
        }

        if (cmd == CMD_STATUS) {
            ts_handle_status(payload, payload_len);
        } else {
            /* válasz-keret: a command()-nak adjuk át a queue-n */
            ts_frame_t f;
            f.cmd = cmd;
            f.payload_len = payload_len;
            if (payload_len) {
                memcpy(f.payload, payload, payload_len);
            }
            /* nem blokkolunk: ha nincs várakozó hívó, eldobjuk az új keret javára */
            if (xQueueSend(s_resp_q, &f, 0) != pdTRUE) {
                xQueueReset(s_resp_q);
                xQueueSend(s_resp_q, &f, 0);
            }
        }
    }
}

/* ======================================================================
 * Publikus API
 * ====================================================================== */
esp_err_t target_serial_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_resp_q = xQueueCreate(2, sizeof(ts_frame_t));
    s_tx_mtx = xSemaphoreCreateMutex();
    if (!s_resp_q || !s_tx_mtx) {
        return ESP_ERR_NO_MEM;
    }

    uart_config_t cfg = {
        .baud_rate = TS_DEFAULT_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(TS_UART_NUM, TS_RX_BUF_SZ, TS_TX_BUF_SZ, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_param_config(TS_UART_NUM, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(TS_UART_NUM, TS_PIN_TX, TS_PIN_RX,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }

    /* induláskor a link még nem áll */
    target_state_set_serial(false);

    if (xTaskCreate(ts_rx_task, "ts_rx", 4096, NULL, 10, &s_rx_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    ESP_LOGI(TAG, "init kész (UART%d TX=%d RX=%d @ %d baud)",
             TS_UART_NUM, TS_PIN_TX, TS_PIN_RX, TS_DEFAULT_BAUD);
    return ESP_OK;
}

esp_err_t target_serial_set_baud(uint32_t baud)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    return uart_set_baudrate(TS_UART_NUM, baud);
}

/* ----------------------------------------------------------------------
 * command(): keret küldés + válasz-keret várás a RX-task queue-ján keresztül.
 *
 * Szinkron mechanizmus: a TX-et mutex sorosítja, így egyszerre csak egy
 * kintlévő parancs van. Küldés előtt ürítjük a válasz-queue-t (stale frame
 * eldobása), majd a queue-ról várjuk a választ timeout_ms-ig. A RX-task a
 * státusz-frame-eket külön kezeli, csak a "nem-státusz" kereteket teszi a
 * queue-ra — így az olvasás egy szálon marad, nincs UART-verseny.
 * -------------------------------------------------------------------- */
esp_err_t target_serial_command(uint8_t cmd, const uint8_t *payload, size_t payload_len,
                                uint8_t *resp, size_t *resp_len, uint32_t timeout_ms)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (payload_len > TS_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_tx_mtx, pdMS_TO_TICKS(timeout_ms + 100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret;
    xQueueReset(s_resp_q);   /* régi válasz eldobása */

    ret = ts_send_frame(cmd, payload, payload_len);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_tx_mtx);
        return ret;
    }

    ts_frame_t f;
    if (xQueueReceive(s_resp_q, &f, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        xSemaphoreGive(s_tx_mtx);
        return ESP_ERR_TIMEOUT;
    }

    if (resp && resp_len) {
        size_t n = f.payload_len;
        if (n > *resp_len) {
            n = *resp_len;   /* a hívó pufferéhez vágjuk */
        }
        memcpy(resp, f.payload, n);
        *resp_len = n;
    } else if (resp_len) {
        *resp_len = 0;
    }

    xSemaphoreGive(s_tx_mtx);
    return ESP_OK;
}

/* segéd: /lfs/cfg/<cfg_name> út összeállítása */
static void ts_cfg_path(const char *cfg_name, char *out, size_t cap)
{
    snprintf(out, cap, STORAGE_LFS_BASE "/cfg/%s", cfg_name);
}

/* ----------------------------------------------------------------------
 * cfg_pull: GET_CONFIG blokkonkénti lekérés, amíg a cél rövidebb (vagy üres)
 * blokkal nem jelzi a végét; az összegyűjtött blob -> /lfs/cfg/<cfg_name>.
 * A GET_CONFIG payload: u16 offset (LE). A válasz payload a kért offsettől
 * vett konfig-bájtokat tartalmazza (CMD_BLOCK_DATA-ig). Ez a cél-protokollhoz
 * igazítandó.
 * -------------------------------------------------------------------- */
esp_err_t target_serial_cfg_pull(const char *cfg_name)
{
    if (!cfg_name) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t  *blob = NULL;
    size_t    blob_len = 0;
    size_t    blob_cap = 0;
    esp_err_t ret = ESP_OK;
    uint16_t  offset = 0;

    for (;;) {
        uint8_t req[CMD_BLOCK_HDR];
        req[0] = (uint8_t)(offset & 0xFF);
        req[1] = (uint8_t)((offset >> 8) & 0xFF);

        uint8_t resp[TS_MAX_PAYLOAD];
        size_t  rlen = sizeof(resp);
        ret = target_serial_command(CMD_GET_CONFIG, req, sizeof(req), resp, &rlen, 1000);
        if (ret != ESP_OK) {
            break;   /* nincs válasz -> ESP_ERR_TIMEOUT propagál */
        }

        if (rlen == 0) {
            break;   /* a cél jelezte: nincs több adat */
        }

        if (blob_len + rlen > blob_cap) {
            size_t new_cap = blob_cap ? blob_cap * 2 : 1024;
            while (new_cap < blob_len + rlen) {
                new_cap *= 2;
            }
            uint8_t *nb = realloc(blob, new_cap);
            if (!nb) {
                ret = ESP_ERR_NO_MEM;
                break;
            }
            blob = nb;
            blob_cap = new_cap;
        }
        memcpy(blob + blob_len, resp, rlen);
        blob_len += rlen;
        offset += (uint16_t)rlen;

        /* a vártnál rövidebb blokk = utolsó blokk */
        if (rlen < CMD_BLOCK_DATA) {
            break;
        }
    }

    if (ret == ESP_OK) {
        char path[128];
        ts_cfg_path(cfg_name, path, sizeof(path));
        ret = storage_lfs_write_all(path, blob ? blob : (const void *)"", blob_len);
    }

    free(blob);
    return ret;
}

/* ----------------------------------------------------------------------
 * cfg_push: /lfs/cfg/<cfg_name> beolvasása, SET_CONFIG keret(ek)ben kiküldése
 * (mindegyik ack-elve), végül COMMIT. A SET_CONFIG payload: u16 offset (LE) +
 * adatblokk. Ez a cél-protokollhoz igazítandó.
 * -------------------------------------------------------------------- */
esp_err_t target_serial_cfg_push(const char *cfg_name)
{
    if (!cfg_name) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[128];
    ts_cfg_path(cfg_name, path, sizeof(path));

    void   *buf = NULL;
    size_t  len = 0;
    esp_err_t ret = storage_lfs_read_all(path, &buf, &len);
    if (ret != ESP_OK) {
        return ret;
    }

    const uint8_t *data = (const uint8_t *)buf;
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > CMD_BLOCK_DATA) {
            chunk = CMD_BLOCK_DATA;
        }

        uint8_t frame_pl[CMD_BLOCK_HDR + CMD_BLOCK_DATA];
        frame_pl[0] = (uint8_t)(offset & 0xFF);
        frame_pl[1] = (uint8_t)((offset >> 8) & 0xFF);
        memcpy(frame_pl + CMD_BLOCK_HDR, data + offset, chunk);

        /* ack-várás: a válasz-payload tartalmát nem nézzük, csak a frame megérkezését */
        ret = target_serial_command(CMD_SET_CONFIG, frame_pl, CMD_BLOCK_HDR + chunk,
                                    NULL, NULL, 1000);
        if (ret != ESP_OK) {
            free(buf);
            return ret;
        }
        offset += chunk;
    }

    /* COMMIT — a cél véglegesíti/elmenti a konfigot */
    ret = target_serial_command(CMD_COMMIT, NULL, 0, NULL, NULL, 2000);

    free(buf);
    return ret;
}
