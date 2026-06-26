#pragma once
/* UART híd a futó cél STM32-höz (sima 3.3 V). (terv 13.)
 *
 * Keretezett bináris protokoll: SOF | LEN | CMD | PAYLOAD | CRC16.
 * Az RX-task a cél periodikus státusz-frame-jeiből frissíti a target_state-et;
 * a .cfg fel/le opak blobként a LittleFS /lfs/cfg-ből/-be.
 * Lábak (terv 2.2): TX=GPIO17, RX=GPIO18, UART1.
 */
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t target_serial_init(void);            /* UART1 + RX task + frame-parszer */

/* Baud állítása (alap 115200). */
esp_err_t target_serial_set_baud(uint32_t baud);

/* .cfg letöltése a célból -> /lfs/cfg/<cfg_name>. */
esp_err_t target_serial_cfg_pull(const char *cfg_name);

/* .cfg feltöltése /lfs/cfg/<cfg_name> -> cél (ack-elve). */
esp_err_t target_serial_cfg_push(const char *cfg_name);

/* Nyers, keretezett parancs küldése és válasz várása (protokoll-adapterhez).
   resp/resp_len lehet NULL ha nem kell válasz-payload. */
esp_err_t target_serial_command(uint8_t cmd, const uint8_t *payload, size_t payload_len,
                                uint8_t *resp, size_t *resp_len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
