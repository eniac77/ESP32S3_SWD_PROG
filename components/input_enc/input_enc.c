#include "input_enc.h"
#include "esp_log.h"

static const char *TAG = "input_enc";

esp_err_t input_enc_init(void)
{
    /* TODO: Gombos enkóder (ISR/PCNT) + nyomógomb (short/long) -> eseménysor (queue) — lásd reference/ESP32S3_SWD_PROG_Plan.md */
    ESP_LOGI(TAG, "stub init");
    return ESP_OK;
}
