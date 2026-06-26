#pragma once
/* SWD programozás orchestráció (C0): a teljes flash-folyamat összekötése.
 *
 * LittleFS fw-fájl -> connect-under-reset -> DEV_ID + flash méret (target_db)
 * -> FLM kiválasztás -> flm_runner erase/program/verify -> reset&run.
 * Progress callbackkel táplálja az OLED-et és a web WebSocketet. (terv 8/16)
 */
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROG_IDLE,
    PROG_CONNECT,   /* connect-under-reset + detektálás */
    PROG_ERASE,
    PROG_PROGRAM,
    PROG_VERIFY,
    PROG_DONE,
    PROG_FAILED,
} prog_phase_t;

typedef struct {
    prog_phase_t phase;
    int          percent;        /* 0..100 az aktuális fázisban */
    uint16_t     dev_id;         /* detektált DEV_ID (0 = ismeretlen) */
    char         target_name[32];/* pl. "STM32F411" vagy "?" */
    char         message[64];    /* állapot/hiba szöveg */
} prog_status_t;

/* Fázis/százalék callback (a flash-elő taszk kontextusában hívódik). */
typedef void (*prog_progress_cb)(const prog_status_t *st, void *ctx);

esp_err_t prog_session_init(void);

/* Szinkron teljes flash egy LittleFS fw-fájlból (pl. "/lfs/fw/app.bin").
   base_addr = 0 esetén az FLM leíró dev_addr-ját használja (tipikusan
   0x08000000). A hívó külön taszkból hívja; egyszerre csak egy futhat
   (belső lock, különben ESP_ERR_INVALID_STATE). A cb lehet NULL. */
esp_err_t prog_session_flash_file(const char *fw_path, uint32_t base_addr,
                                  prog_progress_cb cb, void *ctx);

/* Csak cél-detektálás (connect-under-reset + DEV_ID + flash méret), flash
   nélkül; a végén a célt nyugalmi (reset elengedve) állapotban hagyja. */
esp_err_t prog_session_detect(prog_status_t *out);

/* Folyamatban van-e flash. */
bool prog_session_busy(void);

#ifdef __cplusplus
}
#endif
