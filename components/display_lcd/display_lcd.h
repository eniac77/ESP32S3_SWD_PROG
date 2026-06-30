#pragma once
/*
 * display_lcd.h — ILI9488 480x320 SPI kijelző + GT911 kapacitív touch + LVGL.
 *
 * Az SSD1306/SH1106 OLED (display_oled) leváltása. A panel az esp_lcd
 * SPI-driveren megy (ILI9488, RGB666), a touch a GT911 komponensen, az
 * UI-keretrendszer az LVGL (hivatalos esp_lvgl_port integrációval: render-
 * taszk, tick, lock, flush-callback, touch- és encoder-indev).
 *
 * Lábkiosztás (terv 1. szekció, ILI9488_LVGL_port.md):
 *   LCD:   SCLK=GPIO9, MOSI=GPIO8, CS=GPIO38, DC=GPIO39, RST=GPIO40, BL=GPIO41
 *   Touch: SDA=GPIO47, SCL=GPIO48, INT=GPIO42, RST=GPIO2  (GT911 I2C)
 *
 * EGYELŐRE minimális szerződés (D0 fázis): csak az init-deklaráció. A tényleges
 * panel/touch bring-up és az LVGL-képernyők a D1+ fázisokban kerülnek be.
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SPI busz + ILI9488 panel + GT911 touch + LVGL (esp_lvgl_port) init.
   A main ezt hívja a display_oled_init() helyett. ESP_OK siker esetén.
   D0: STUB — még nem konfigurál HW-t, csak logol. */
esp_err_t display_lcd_init(void);

#ifdef __cplusplus
}
#endif
