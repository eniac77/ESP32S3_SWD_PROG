#include "target_db.h"
#include "esp_log.h"

static const char *TAG = "target_db";

esp_err_t target_db_init(void)
{
    /* TODO: Cél-adatbázis: DEV_ID -> FLM + flash-size reg (STM32 F0/F1/F3/F4/F7/L0/L1/L4/G0) — lásd reference/ESP32S3_SWD_PROG_Plan.md */
    ESP_LOGI(TAG, "stub init");
    return ESP_OK;
}
