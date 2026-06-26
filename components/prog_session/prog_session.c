#include "prog_session.h"
#include "esp_log.h"

static const char *TAG = "prog_session";

esp_err_t prog_session_init(void)
{
    /* TODO: SWD programozás orchestráció: connect-under-reset -> erase -> program -> verify, progress callback — lásd reference/ESP32S3_SWD_PROG_Plan.md */
    ESP_LOGI(TAG, "stub init");
    return ESP_OK;
}
