#include "target_state.h"
#include "esp_log.h"

static const char *TAG = "target_state";

esp_err_t target_state_init(void)
{
    /* TODO: Közös élő-adat modell (mutex/atomic snapshot): OLED és WebSocket forrása — lásd reference/ESP32S3_SWD_PROG_Plan.md */
    ESP_LOGI(TAG, "stub init");
    return ESP_OK;
}
