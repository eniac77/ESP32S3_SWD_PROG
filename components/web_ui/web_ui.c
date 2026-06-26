#include "web_ui.h"
#include "esp_log.h"

static const char *TAG = "web_ui";

esp_err_t web_ui_init(void)
{
    /* TODO: esp_http_server: REST API + WebSocket (élő adat + programozási progress) — lásd reference/ESP32S3_SWD_PROG_Plan.md */
    ESP_LOGI(TAG, "stub init");
    return ESP_OK;
}
