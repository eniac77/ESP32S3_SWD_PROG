#include "display_oled.h"
#include "esp_log.h"

static const char *TAG = "display_oled";

esp_err_t display_oled_init(void)
{
    /* TODO: SSD1306/SH1106 128x64 I2C driver + helyi UI képernyők (framebuffer) — lásd reference/ESP32S3_SWD_PROG_Plan.md */
    ESP_LOGI(TAG, "stub init");
    return ESP_OK;
}
