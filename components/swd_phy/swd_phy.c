#include "swd_phy.h"
#include "esp_log.h"

static const char *TAG = "swd_phy";

esp_err_t swd_phy_init(void)
{
    /* TODO: SWD fizikai réteg (dedic_gpio HAL): seq_out/seq_in/dir/idle — platformspecifikus — lásd reference/ESP32S3_SWD_PROG_Plan.md */
    ESP_LOGI(TAG, "stub init");
    return ESP_OK;
}
