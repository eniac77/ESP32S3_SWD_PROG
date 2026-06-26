#include "flm_blobs.h"
#include "esp_log.h"

static const char *TAG = "flm_blobs";

esp_err_t flm_blobs_init(void)
{
    /* TODO: Generált FLM C tömbök (PrgCode/PrgData/DevDsc) — build-time flm_extract.py tölti — lásd reference/ESP32S3_SWD_PROG_Plan.md */
    ESP_LOGI(TAG, "stub init");
    return ESP_OK;
}
