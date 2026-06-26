#include "flm_runner.h"
#include "esp_log.h"

static const char *TAG = "flm_runner";

esp_err_t flm_runner_init(void)
{
    /* TODO: CMSIS FLM futtató: call_function a cél RAM-jában (Init/Erase/Program/Verify) — lásd reference/ESP32S3_SWD_PROG_Plan.md */
    ESP_LOGI(TAG, "stub init");
    return ESP_OK;
}
