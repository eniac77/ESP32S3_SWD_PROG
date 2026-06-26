#include "ftp_srv.h"
#include "esp_log.h"

static const char *TAG = "ftp_srv";

esp_err_t ftp_srv_init(void)
{
    /* TODO: FTP szerver a LittleFS fölött (fw/cfg fel-/letöltés, storage lock) — lásd reference/ESP32S3_SWD_PROG_Plan.md */
    ESP_LOGI(TAG, "stub init");
    return ESP_OK;
}
