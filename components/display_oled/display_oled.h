#pragma once
/*
 * display_oled.h — SSD1306 / SH1106 128x64 I2C OLED driver.
 *
 * Az NX80TESTER (AVR) ssd1306.c / display.c / mikrofont.c kódjának portja
 * ESP-IDF-re. Az új driver/i2c_master API-t használja (i2c_master_bus +
 * i2c_master_dev), statikus 1 KB framebufferrel és laponkénti flush-sal.
 *
 * Panel-konfiguráció a display_oled.c tetején lévő #define-okkal:
 *   - oszlop-offset (SSD1306=0 / SH1106=2)
 *   - 180° forgatás (seg remap + COM scan irány)
 */
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Panel-méret */
#define OLED_W 128
#define OLED_H 64

/* ---------------------------------------------------------------------- */
/* Inicializálás / panel-vezérlés                                         */
/* ---------------------------------------------------------------------- */

/* I2C busz + eszköz létrehozás, panel init, clear+flush, boot felirat.
   A main ezt hívja. ESP_OK siker esetén. */
esp_err_t display_oled_init(void);

/* Display be/ki (parancs). */
void display_oled_on(void);
void display_oled_off(void);

/* ---------------------------------------------------------------------- */
/* Framebuffer / rajz-primitívek (NX80 ssd1306.c port)                    */
/* ---------------------------------------------------------------------- */

/* Teljes framebuffer törlése (csak a memóriát; flush külön). */
void display_oled_clear(void);

/* Framebuffer kiküldése a panelre, laponként (0..7), laponként 1 I2C
   tranzakció: control 0x40 + 128 adatbájt. */
void display_oled_flush(void);

/* Egy pixel beállítása / törlése. */
void display_oled_pixel(uint8_t x, uint8_t y, bool on);

/* Kitöltött téglalap. */
void display_oled_fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool on);

/* 1px vastag keret (téglalap körvonal). */
void display_oled_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h);

/* ---------------------------------------------------------------------- */
/* Szövegrajz — 5x7 ASCII font, egész skálával nagyítva (NX80 display.c)  */
/* ---------------------------------------------------------------------- */

/* Egy karakter rajzolása (scale=1,2,...). */
void    display_oled_char(uint8_t x, uint8_t y, char c, uint8_t scale, bool on);

/* String rajzolása balról (scale-szeres méret, on=true). */
void    display_oled_text(uint8_t x, uint8_t y, const char *s, uint8_t scale);

/* String pixel-szélessége adott skálán. */
uint8_t display_oled_text_width(const char *s, uint8_t scale);

/* String vízszintesen középre rajzolva. */
void    display_oled_text_center(uint8_t y, const char *s, uint8_t scale);

/* ---------------------------------------------------------------------- */
/* Nagy szám-font — Swis721 19x24 (NX80 mikrofont.c port)                 */
/* Csak '.', '/', 0-9, ':' karakterek a számokhoz/százalékhoz.            */
/* ---------------------------------------------------------------------- */

/* Egy nagy karakter; visszaadja a (változó) szélességét px-ben. */
uint8_t display_oled_big_char(uint8_t x, uint8_t y, char c);

/* Nagy string rajzolása (1px köz a karakterek között). */
void    display_oled_big_str(uint8_t x, uint8_t y, const char *s);

/* Nagy string teljes pixel-szélessége. */
uint8_t display_oled_big_str_width(const char *s);

/* Nagy string vízszintesen középre. */
void    display_oled_big_center(uint8_t y, const char *s);

#ifdef __cplusplus
}
#endif
