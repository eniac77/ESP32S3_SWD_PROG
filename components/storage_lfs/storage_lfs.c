#include "storage_lfs.h"
#include "esp_log.h"

static const char *TAG = "storage_lfs";

esp_err_t storage_lfs_init(void)
{
    /* TODO: LittleFS mount + fájl-API + közös lock (FONTOS: a tényleges littlefs függőséget KÉSŐBB adjuk hozzá, most csak stub) — lásd reference/ESP32S3_SWD_PROG_Plan.md */
    ESP_LOGI(TAG, "stub init");
    return ESP_OK;
}
