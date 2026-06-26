#pragma once
/*
 * input_enc — Gombos enkóder bevitel ESP32-S3 (ESP-IDF v5.5.1)
 *
 * Enkóder: hardveres kvadratúra-dekód a PCNT (pulse counter) periférián,
 * glitch-filterrel, ISR-jitter nélkül (terv 14.1, ajánlott út).
 * Gomb: GPIO bemenet belső pullup-pal, esp_timer-es pollozott debounce,
 * rövid/hosszú nyomás megkülönböztetéssel (terv 14.2).
 *
 * A források (enkóder-poll, gomb-poll) egy közös FreeRTOS queue-ba teszik az
 * eseményeket; a fogyasztó (ui_task) az input_enc_get()-tel olvas (terv 14.3).
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bevitel-események (terv 14.3). */
typedef enum {
    ENC_CW,     /* enkóder egy detent az óramutató járásával megegyezően */
    ENC_CCW,    /* enkóder egy detent az óramutató járásával ellentétesen */
    BTN_SHORT,  /* rövid gombnyomás (felengedéskor, ha < küszöb) */
    BTN_LONG    /* hosszú gombnyomás (a nyomvatartás eléri a küszöböt) */
} enc_event_t;

/* Komponens inicializálás: PCNT + gomb + esp_timer(ek) + queue felállítása.
   A main ezt hívja. ESP_OK siker esetén. */
esp_err_t input_enc_init(void);

/* Esemény kiolvasása a sorból; ticks_to_wait=portMAX_DELAY blokkol.
   true=kaptunk eseményt (*ev kitöltve), false=timeout. */
bool input_enc_get(enc_event_t *ev, uint32_t ticks_to_wait);

#ifdef __cplusplus
}
#endif
