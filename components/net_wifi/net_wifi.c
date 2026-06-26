#include "net_wifi.h"
#include "esp_log.h"

static const char *TAG = "net_wifi";

esp_err_t net_wifi_init(void)
{
    /* TODO: WiFi STA/AP (APSTA), provisioning NVS-ből, mDNS (swdprog.local) — lásd reference/ESP32S3_SWD_PROG_Plan.md */
    ESP_LOGI(TAG, "stub init");
    return ESP_OK;
}
