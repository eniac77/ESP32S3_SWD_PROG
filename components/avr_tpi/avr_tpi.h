#pragma once
/* AVR TPI programozó (reduced-core ATtiny4/5/9/10/20/40/102/104).
 *
 * A TPI (Tiny Programming Interface) egy szinkron, framelt (12-bites, páros
 * paritás), a PDI-hoz nagyon hasonló interfész, de HÁROM vezetékkel:
 *   TPICLK   = órajel (a programozó adja)      -> Kconfig default SCK/GPIO15
 *   TPIDATA  = kétirányú adat (turnaround)      -> Kconfig default MISO/GPIO7
 *   RESET    = a cél RESET lába (alacsonyan tartva a TPI alatt) -> GPIO21
 *
 * Teljesen független a SWD/FLM, ISP, UPDI és PDI magtól. A flashelés a TPI
 * link-rétegen (SLD/SST/SSTPR/SLDCS/SSTCS/SKEY) és a reduced-core tiny NVM-
 * vezérlőn (WORD_WRITE, CHIP_ERASE) át megy. Forrás: .hex/.elf/.bin (avr_image).
 *
 * FIGYELEM: HW-n MÉG NEM IGAZOLT. Lásd: reference/AVR_TPI.md.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Detektált TPI (reduced-core tiny) cél leírója. */
typedef struct {
    uint8_t     sig[3];     /* signature, pl. ATtiny10: 1E 90 03 */
    const char *name;       /* eszköznév vagy NULL ha ismeretlen */
    uint32_t    flash_size; /* flash méret bájtban */
    uint16_t    page_size;  /* flash szekció-/lapméret bájtban (erase granularitás) */
    bool        known;      /* true, ha a signature szerepel a táblában */
} avr_tpi_dev_t;

/* Bit-bang TPI PHY felhozása (Kconfig-lábak). Idempotens. */
esp_err_t avr_tpi_init(void);

/* TPI engedélyezés + signature kiolvasás -> kilépés. out kitöltve. */
esp_err_t avr_tpi_detect(avr_tpi_dev_t *out);

/* Fázis/százalék callback (a flashelő taszk kontextusából hívódik). */
typedef void (*avr_tpi_progress_cb)(const char *phase, int percent, void *ctx);

/* Forrásfájl flashelése TPI-n. A kiterjesztés dönt (.hex/.elf/.bin).
   Folyamat: enable -> NVM key -> chip erase -> word program -> verify -> reset. */
esp_err_t avr_tpi_flash_file(const char *path, avr_tpi_progress_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif
