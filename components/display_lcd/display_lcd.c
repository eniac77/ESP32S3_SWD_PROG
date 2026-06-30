/*
 * display_lcd.c — ILI9488 SPI + GT911 touch + LVGL init (D0: STUB).
 *
 * Ez a fájl a D0 fázisban csak a managed függőségek feloldását és fordulását
 * bizonyítja: include-olja a négy managed header-t, hogy a build-rendszer
 * letöltse és összelinkelje őket. A tényleges HW bring-up (SPI busz, panel,
 * touch, LVGL-indevek) a D1+ fázisban kerül ide.
 */
#include "display_lcd.h"
#include "esp_log.h"

/* Managed függőségek — a D0 célja, hogy ezek bizonyítottan feloldódjanak.
   (A bevont szimbólumokat a D1 fázis fogja használni.) */
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_ili9488.h"
#include "esp_lcd_touch_gt911.h"

static const char *TAG = "display_lcd";

esp_err_t display_lcd_init(void)
{
    /* D0 STUB: még nincs HW-konfiguráció, hogy ne ütközzön az OLED lábaival.
       A main szándékosan NEM hívja ezt a fázisban — csak a fordulás a cél. */
    ESP_LOGI(TAG, "display_lcd_init STUB (D0) — LVGL %d.%d.%d behúzva",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    return ESP_OK;
}
