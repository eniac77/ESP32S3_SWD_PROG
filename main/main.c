/*
 * ESP32-S3 önálló SWD programozó + konfigurátor — belépési pont.
 *
 * Teljes UART-konzol naplózás: boot-banner + rendszer-infó, komponens-init
 * eredmények, és (Kconfig: BRINGUP_SELFTEST) boot-idői SWD önteszt, ami a
 * csatlakoztatott célt detektálja és kiírja a DPIDR/DEV_ID/flash/RDP adatokat.
 * A részletes SWD/FLM/flash naplót az alsó rétegek adják (DEBUG/VERBOSE szint).
 */
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_timer.h"
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

/* Init-hívás eredményének naplózása (siker is, hiba is — minden fontos). */
#define TRY(call) do {                                          \
        int64_t _t0 = esp_timer_get_time();                     \
        esp_err_t _e = (call);                                  \
        int64_t _dt = (esp_timer_get_time() - _t0) / 1000;      \
        if (_e == ESP_OK) {                                     \
            ESP_LOGI(TAG, "  [OK ] %-22s (%lld ms)", #call, _dt); \
        } else {                                                \
            ESP_LOGW(TAG, "  [ERR] %-22s -> %s", #call,         \
                     esp_err_to_name(_e));                      \
        }                                                       \
    } while (0)

/* A mag/SWD/FLM rétegek tag-jeit a legrészletesebb szintre emeljük, hogy a
   teljes bring-up/flash folyamat látszódjon az UART-konzolon. */
static void setup_logging(void)
{
    esp_log_level_set("*",            ESP_LOG_INFO);
    const char *verbose_tags[] = {
        "swd_phy", "adiv5", "cortexm", "flm_runner", "flm_blobs",
        "target_db", "prog_session", "target_serial", "ui",
    };
    for (size_t i = 0; i < sizeof(verbose_tags) / sizeof(verbose_tags[0]); i++) {
        esp_log_level_set(verbose_tags[i], ESP_LOG_VERBOSE);
    }
}

/* Boot-banner + rendszer-infó. */
static void log_banner(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " ESP32-S3 SWD programozo + konfigurator");
    ESP_LOGI(TAG, " ESP-IDF: %s", esp_get_idf_version());
    ESP_LOGI(TAG, " Chip: ESP32-S3 rev %d.%d, %d mag, WiFi%s%s",
             chip.revision / 100, chip.revision % 100, chip.cores,
             (chip.features & CHIP_FEATURE_BT) ? "+BT" : "",
             (chip.features & CHIP_FEATURE_BLE) ? "+BLE" : "");
    ESP_LOGI(TAG, " Heap szabad: %u B (belso), PSRAM szabad: %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "==================================================");
}

/* Boot-idői SWD önteszt: a csatlakoztatott cél detektálása + teljes napló. */
#if CONFIG_BRINGUP_SELFTEST
static void bringup_selftest(void)
{
    prog_status_t st;
    ESP_LOGI(TAG, "[SELFTEST] SWD cel-detektalas indul (nRST nelkul)...");
    esp_err_t err = prog_session_detect(&st);

    if (err == ESP_OK && st.dev_id != 0) {
        ESP_LOGI(TAG, "[SELFTEST] CEL OK: %s", st.target_name);
        ESP_LOGI(TAG, "[SELFTEST]   DEV_ID = 0x%03X", st.dev_id);
        ESP_LOGI(TAG, "[SELFTEST]   info   = %s", st.message);
    } else {
        ESP_LOGW(TAG, "[SELFTEST] nincs cel / hiba: %s (%s)",
                 st.message[0] ? st.message : "-", esp_err_to_name(err));
        ESP_LOGW(TAG, "[SELFTEST] ellenorizd: SWCLK=GPIO4->PA14, SWDIO=GPIO5->PA13, "
                      "GND kozos, cel 3.3V; nRST nem kell.");
    }
}
#endif

void app_main(void)
{
    setup_logging();
    log_banner();

    ESP_LOGI(TAG, "Komponensek inicializalasa:");

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

    ESP_LOGI(TAG, "Init kesz. Fobciklus indul.");

#if CONFIG_BRINGUP_SELFTEST
    bringup_selftest();
#endif

    uint32_t tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

#if CONFIG_BRINGUP_SELFTEST_PERIODIC
        /* Periodikus újra-detektálás (bench-hez): N mp-enként. */
        if ((++tick % CONFIG_BRINGUP_SELFTEST_PERIOD_S) == 0) {
            bringup_selftest();
        }
#else
        /* Heartbeat: ~30 mp-enként szabad-heap napló (életjel). */
        if ((++tick % 30) == 0) {
            ESP_LOGI(TAG, "[heartbeat] up=%lus, heap=%uB, psram=%uB",
                     (unsigned long)(tick),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
#endif
    }
}
