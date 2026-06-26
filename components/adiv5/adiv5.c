#include "adiv5.h"
#include "esp_log.h"

static const char *TAG = "adiv5";

esp_err_t adiv5_init(void)
{
    /* TODO: ADIv5 transport: DP/AP, JTAG-to-SWD switch, ACK/parity, mem R/W — lásd reference/ESP32S3_SWD_PROG_Plan.md */
    ESP_LOGI(TAG, "stub init");
    return ESP_OK;
}
