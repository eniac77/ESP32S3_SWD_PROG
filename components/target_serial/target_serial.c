#include "target_serial.h"
#include "esp_log.h"

static const char *TAG = "target_serial";

esp_err_t target_serial_init(void)
{
    /* TODO: UART híd a futó cél STM32-höz (3.3V): keretezett protokoll, .cfg fel/le, élő adat — lásd reference/ESP32S3_SWD_PROG_Plan.md */
    ESP_LOGI(TAG, "stub init");
    return ESP_OK;
}
