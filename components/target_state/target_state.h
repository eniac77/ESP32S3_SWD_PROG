#pragma once
/* Közös élő-adat modell — mutex-elt snapshot. (terv 13.3)
 *
 * Egy forrás, két nézet: a target_serial írja (a cél státusz-frame-jeiből),
 * a ui_task (OLED) és a web_ui (WebSocket) olvassa.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TARGET_STATE_MAX_VALUES 8

typedef struct {
    bool     target_present;                 /* SWD-n detektált cél */
    uint16_t dev_id;                         /* detektált DEV_ID (0 = ismeretlen) */
    char     target_name[32];                /* pl. "STM32F411" */
    bool     serial_link;                    /* a soros link él-e */
    uint32_t uptime_s;                       /* cél app uptime (ha küldi) */
    float    values[TARGET_STATE_MAX_VALUES];/* generikus élő értékek */
    uint8_t  value_count;
    int64_t  last_update_us;                 /* esp_timer_get_time az utolsó frissítéskor */
} target_state_t;

esp_err_t target_state_init(void);

/* Mutex-elt teljes snapshot kiolvasása. */
void target_state_get(target_state_t *out);

/* Részleges, mutex-elt frissítők (a forrás ezeket hívja). */
void target_state_set_target(bool present, uint16_t dev_id, const char *name);
void target_state_set_serial(bool up);
void target_state_set_values(const float *vals, uint8_t n, uint32_t uptime_s);

#ifdef __cplusplus
}
#endif
