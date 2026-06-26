/*
 * ESP32-S3 önálló SWD programozó + konfigurátor — belépési pont.
 *
 * Ez a skeleton csak inicializálja (stub szinten) a komponenseket és naplóz.
 * A tényleges működést az egyes komponensek fokozatosan kapják meg
 * (lásd reference/ESP32S3_SWD_PROG_Plan.md fázisos ütemterv).
 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "storage_lfs.h"
#include "display_oled.h"
#include "input_enc.h"
#include "net_wifi.h"
#include "web_ui.h"
#include "ftp_srv.h"
#include "target_serial.h"
#include "target_state.h"
#include "prog_session.h"
#include "target_db.h"
#include "swd_phy.h"
#include "adiv5.h"
#include "cortexm_debug.h"
#include "flm_runner.h"
#include "flm_blobs.h"
#include "ui.h"

static const char *TAG = "main";

#define TRY(call) do {                                  \
        esp_err_t _e = (call);                          \
        if (_e != ESP_OK) {                             \
            ESP_LOGW(TAG, "%s -> %s", #call,            \
                     esp_err_to_name(_e));              \
        }                                               \
    } while (0)

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 SWD programozo + konfigurator — boot");

    /* Platform alapok */
    TRY(storage_lfs_init());
    TRY(target_state_init());

    /* Helyi UI */
    TRY(display_oled_init());
    TRY(input_enc_init());
    TRY(ui_start());

    /* Cél-interfészek (külön lábakon) */
    TRY(swd_phy_init());
    TRY(adiv5_init());
    TRY(cortexm_debug_init());
    TRY(flm_runner_init());
    TRY(flm_blobs_init());
    TRY(target_db_init());
    TRY(prog_session_init());
    TRY(target_serial_init());

    /* Hálózat / távoli elérés */
    TRY(net_wifi_init());
    TRY(web_ui_init());
    TRY(ftp_srv_init());

    ESP_LOGI(TAG, "init kesz (skeleton); fobciklus indul");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
