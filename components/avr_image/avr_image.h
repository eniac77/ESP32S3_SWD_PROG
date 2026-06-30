#pragma once
/* AVR firmware-kép betöltő — közös réteg az ISP/PDI/UPDI flow-khoz.
 *
 * A forrásfájl nyers bájtjaiból (amit a hívó már beolvasott a storage_src-ből)
 * egy lapos flash-képet épít. A kiterjesztés dönt:
 *   ".hex" -> Intel HEX, ".elf" -> AVR ELF32-LE, egyébként nyers .bin (0-tól).
 *
 * Az interfész-magoktól (ISP bit-bang, UPDI UART, PDI bit-bang) független, tisztán
 * memóriában dolgozik — így a három programozó ugyanazt a parsert használja.
 */
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A 'raw'/'raw_len' nyers forrásból flash-képet épít az 'img' pufferbe.
 *   path     : csak a kiterjesztés-egyezéshez (.hex/.elf/egyéb).
 *   img      : a hívó által foglalt, img_size bájtos puffer, 0xFF-fel előtöltve
 *              (törölt flash); a parser ide írja a hasznos tartalmat.
 *   out_len  : a legmagasabb beírt cím + 1 (a tényleges flashelendő hossz).
 * Hiba esetén esp_err_t (pl. ESP_ERR_INVALID_SIZE, ha a kép nem fér a flash-be). */
esp_err_t avr_image_parse(const char *path, const void *raw, size_t raw_len,
                          uint8_t *img, size_t img_size, size_t *out_len);

#ifdef __cplusplus
}
#endif
