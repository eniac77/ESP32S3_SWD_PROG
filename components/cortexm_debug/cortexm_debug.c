#include "cortexm_debug.h"
#include "esp_log.h"

static const char *TAG = "cortexm_debug";

esp_err_t cortexm_debug_init(void)
{
    /* TODO: Cortex-M debug: halt/reset, connect-under-reset, core reg, mem R/W — lásd reference/ESP32S3_SWD_PROG_Plan.md */
    ESP_LOGI(TAG, "stub init");
    return ESP_OK;
}
