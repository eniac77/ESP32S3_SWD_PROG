/* Közös élő-adat modell — mutex-elt snapshot implementáció. (terv 13.3)
 *
 * Egyetlen statikus állapot (s_state) FreeRTOS mutexszel védve. Minden
 * get/set rövid kritikus szakaszban dolgozik: a get egy memcpy snapshotot
 * ad vissza, a setterek csak a releváns mezőket írják.
 */
#include "target_state.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

/* A védett, globális állapot. */
static target_state_t   s_state;
/* A hozzáférést sorosító mutex. */
static SemaphoreHandle_t s_mutex;

/* Mutex bekérése — init után mindig érvényes a handle. */
static inline void state_lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

/* Mutex elengedése. */
static inline void state_unlock(void)
{
    xSemaphoreGive(s_mutex);
}

esp_err_t target_state_init(void)
{
    /* Mutex létrehozása. */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Kezdeti, "üres" állapot: nincs cél, nincs soros link, nincsenek értékek. */
    memset(&s_state, 0, sizeof(s_state));
    s_state.target_present = false;
    s_state.serial_link    = false;
    s_state.value_count    = 0;

    return ESP_OK;
}

void target_state_get(target_state_t *out)
{
    if (out == NULL) {
        return;
    }

    /* Teljes snapshot kimásolása a hívó pufferébe a mutex alatt. */
    state_lock();
    memcpy(out, &s_state, sizeof(s_state));
    state_unlock();
}

void target_state_set_target(bool present, uint16_t dev_id, const char *name)
{
    state_lock();

    s_state.target_present = present;
    s_state.dev_id         = dev_id;

    /* Név biztonságos másolása: legfeljebb a puffer méreténél eggyel kevesebb
     * karakter, majd kötelező NUL-terminálás. NULL név esetén üres string. */
    if (name != NULL) {
        strncpy(s_state.target_name, name, sizeof(s_state.target_name) - 1);
        s_state.target_name[sizeof(s_state.target_name) - 1] = '\0';
    } else {
        s_state.target_name[0] = '\0';
    }

    s_state.last_update_us = esp_timer_get_time();

    state_unlock();
}

void target_state_set_serial(bool up)
{
    state_lock();

    s_state.serial_link    = up;
    s_state.last_update_us = esp_timer_get_time();

    state_unlock();
}

void target_state_set_values(const float *vals, uint8_t n, uint32_t uptime_s)
{
    /* A bejövő darabszám TARGET_STATE_MAX_VALUES-re vágva. */
    if (n > TARGET_STATE_MAX_VALUES) {
        n = TARGET_STATE_MAX_VALUES;
    }

    state_lock();

    if (vals != NULL && n > 0) {
        memcpy(s_state.values, vals, (size_t)n * sizeof(s_state.values[0]));
    }
    s_state.value_count    = n;
    s_state.uptime_s       = uptime_s;
    s_state.last_update_us = esp_timer_get_time();

    state_unlock();
}
