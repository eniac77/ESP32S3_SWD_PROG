#pragma once
/* AVR ISP programozó (ATtiny13 + rokon AVR-ek) — bit-bang SPI ISP.
 *
 * Teljesen független a SWD/FLM magtól: az AVR-eket SPI-szerű ISP protokollon
 * programozzuk, a célt RESET alatt tartva. Forrás LittleFS-ből: Intel HEX
 * (.hex) vagy nyers flash-kép (.bin).
 *
 * Lábak (default, Kconfig felülírhatja): SCK=GPIO15, MOSI=GPIO16,
 * MISO=GPIO7, RESET=GPIO21. (Mind szabad láb — nem ütközik a SWD/UART/OLED/
 * enkóder kiosztással.) A RESET aktív-alacsony, programozás alatt lehúzva.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Detektált AVR cél leírója. */
typedef struct {
    uint8_t     sig[3];     /* signature bájtok, pl. 0x1E 0x90 0x07 (ATtiny13) */
    const char *name;       /* eszköznév vagy NULL ha ismeretlen */
    uint32_t    flash_size; /* flash méret bájtban */
    uint16_t    page_size;  /* flash lapméret bájtban (page write granularitás) */
    bool        known;      /* true, ha a signature szerepel a táblában */
} avr_dev_t;

/* GPIO-k konfigurálása (SCK/MOSI kimenet, MISO bemenet, RESET kimenet). */
esp_err_t avr_isp_init(void);

/* ISP módba lépés -> signature kiolvasás -> kilépés. out kitöltve.
   Ismeretlen signature esetén known=false, name=NULL (a sig akkor is megvan). */
esp_err_t avr_isp_detect(avr_dev_t *out);

/* Fázis/százalék callback (a flashelő taszk kontextusában hívódik). */
typedef void (*avr_progress_cb)(const char *phase, int percent, void *ctx);

/* LittleFS fájl flashelése ISP-n. A kiterjesztés dönt: ".hex" -> Intel HEX
   parse, egyébként nyers .bin (0-tól a flash-be). Folyamat:
   enter -> chip erase -> page program -> verify -> leave. cb lehet NULL. */
esp_err_t avr_isp_flash_file(const char *path, avr_progress_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif
