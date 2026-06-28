/*
 * oled_fonts.h — Font-adatok és font-elérő API az OLED driverhez.
 *
 * Az NX80TESTER (AVR) font.c és font_swis721.h fájljaiból portolva.
 * A PROGMEM / pgm_read_byte AVR-specifikus tárolás helyett sima C tömbök
 * vannak (a flash-ben ott élnek a `const` adatok, közvetlen indexeléssel).
 *
 * Két font:
 *   1) 5x7 ASCII font (0x20..0x7E, teljes nyomtatható ASCII) — menü/feliratokhoz, skálázva rajzolható.
 *   2) Swis721 19x24 nagy szám-font (char 46..58: '.', '/', 0-9, ':') — a
 *      számláló / százalék / MM:SS kijelzéshez.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 5x7 ASCII font ---- */
/* 96 glyph, 0x20..0x7F (a 0x7F üres), 5 oszlop / karakter, oszloponként alsó bit = felső pixel. */
extern const uint8_t OLED_FONT5X7[96][5];

/* A kért karakter 5 bájtos oszlop-adatára mutató pointer (nem támogatott -> szóköz). */
const uint8_t *oled_font_glyph(char c);

/* ---- Swis721 19x24 nagy szám-font ---- */
/* X-GLCD formátum: karakterenként 1 fejléc-bájt (változó szélesség) + width*3 adatbájt
   (a magasság 24 -> 3 bájt/oszlop). Char-tartomány: 46 ('.') .. 58 (':'). */
extern const uint8_t OLED_FONT_SWIS721_19x24[];

#define OLED_BIGFONT_WIDTH   19   /* max szélesség (stride) */
#define OLED_BIGFONT_HEIGHT  24   /* magasság px */
#define OLED_BIGFONT_START   46   /* '.' */
#define OLED_BIGFONT_END     58   /* ':' */

#ifdef __cplusplus
}
#endif
