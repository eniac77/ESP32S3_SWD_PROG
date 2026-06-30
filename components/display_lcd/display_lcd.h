#pragma once
/*
 * display_lcd.h — ILI9488 480x320 SPI kijelző + GT911 kapacitív touch + LVGL.
 *
 * Az SSD1306/SH1106 OLED (display_oled) leváltása. A panel az esp_lcd
 * SPI-driveren megy (ILI9488, RGB666 — a panel-driver RGB565->RGB666 konvertál),
 * a touch a GT911 komponensen, az UI-keretrendszer az LVGL v9 (hivatalos
 * esp_lvgl_port 2.8 integrációval: render-taszk, tick, lock, flush-callback,
 * touch-indev). Az enkóder EGYEDI LVGL encoder-indevként van bekötve (a
 * meglévő input_enc queue-t fogyasztja — lásd display_lcd.c).
 *
 * Lábkiosztás (terv 1. szekció, ILI9488_LVGL_port.md):
 *   LCD:   SCLK=GPIO9, MOSI=GPIO8, CS=GPIO38, DC=GPIO39, RST=GPIO40, BL=GPIO41
 *   Touch: SDA=GPIO47, SCL=GPIO48, INT=GPIO42, RST=GPIO2  (GT911 I2C, I2C_NUM_0)
 *
 * !!! HW-n MÉG NEM IGAZOLT — D1/D2/D4 HW-teszt hátravan. !!!
 * A kód zöldre fordul, de a panel/touch/orientáció/szín valódi kijelzőn nincs
 * validálva. Az orientáció (swap_xy/mirror) és a SPI-frekvencia HW-n hangolandó.
 *
 * --- API-szerződés a D3 (UI-port) számára ---
 * A D3 az alábbi 3 függvényt + a lvgl_port_lock/unlock-ot használja. NINCS
 * UI-logika ebben a komponensben; a D3 építi a képernyőket az LVGL widgetekkel.
 *
 * LVGL thread-safety: az LVGL NEM thread-safe a saját render-taszkján kívül.
 * A D3 (és bármely más taszk) MINDEN LVGL-hívást a port-lock alatt végezzen:
 *     if (lvgl_port_lock(0)) { ... lv_*() ... lvgl_port_unlock(); }
 * (0 = végtelen várakozás.) Ezt szándékosan NEM burkoljuk újra.
 */
#include "esp_err.h"

/* LVGL-típusok (lv_group_t) az API-hoz. Az lvgl behúzza a v9 fejléceket. */
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SPI busz + ILI9488 panel + GT911 touch + LVGL (esp_lvgl_port) teljes init.
   Felhúzza az SPI2 buszt, a panelt, a touch I2C-buszt, az LVGL-portot és a
   render-taszkot, valamint a touch- és enkóder-indeveket. A main ezt hívja a
   display_oled_init() helyett (a D5 köti be — D1/D2 alatt a main NEM hívja).
   ESP_OK siker esetén. */
esp_err_t display_lcd_init(void);

/* Az enkóder-navigációs LVGL csoport. A D3 ebbe veszi a fókuszálható
   widgeteket (lv_group_add_obj), így az enkóder forgatása lépteti a fókuszt,
   a rövid gombnyomás aktivál. A csoport a display_lcd_init() után érvényes;
   init előtt / hibás init után NULL.
   Használat (D3): lv_group_add_obj(display_lcd_group(), my_btn); */
lv_group_t *display_lcd_group(void);

/* A "vissza" (BTN_LONG, hosszú gombnyomás) eseményre meghívandó callback.
   Az LVGL encoder-indevnek nincs natív "back" fogalma, ezért a hosszú nyomást
   külön kezeljük: a regisztrált cb az LVGL render-taszk kontextusában,
   az indev read_cb-ből hívódik (port-taszk) — a cb-ben szabad közvetlenül
   lv_*()-t hívni (NE vegyél benne újra port-lock-ot). cb=NULL kikapcsolja.
   Tipikus használat (D3): a cb lépjen vissza az előző képernyőre. */
void display_lcd_set_back_cb(void (*cb)(void));

/* A háttérvilágítás (BL, GPIO41) fényerejének állítása százalékban (0..100).
   A BL LEDC PWM-en megy (alap 50%); ez a setter az LEDC duty-t állítja
   (clamp 0..100). Hasznos a UI-ból / későbbi beállítás-képernyőről. Ha a
   display_lcd_init() még nem futott le (vagy hibára futott), a hívás no-op.
   Megjegyzés: aktív-magas BL feltételezve — ha a HW fordított, a fényerő
   iránya megfordul (a duty-számítást kell invertálni a .c-ben). */
void display_lcd_set_brightness(uint8_t pct);

#ifdef __cplusplus
}
#endif
