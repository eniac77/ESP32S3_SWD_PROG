#pragma once
/* AVR PDI programozó (XMEGA család — ATxmega...).
 *
 * A PDI (Program and Debug Interface) KÉT vezetékes, szinkron, framelt (12-bites,
 * páros paritás) interfész:
 *   PDI_CLK  = a target RESET lába (a programozó adja az órajelet)
 *   PDI_DATA = kétirányú adat (turnaround a SWDIO-hoz hasonlóan)
 *
 * A meglévő "pines" programozó-headeren: PDI_CLK = RESET-vonal (Kconfig default
 * GPIO21), PDI_DATA = MISO-vonal (Kconfig default GPIO7). SCK/MOSI nem használt.
 *
 * Teljesen független a SWD/FLM, az ISP (avr_isp) és az UPDI (avr_updi) magtól.
 * A flashelés a PDI link-rétegen (LDS/STS/LDCS/STCS/KEY) és az XMEGA NVM-vezérlőn
 * át megy. Forrás LittleFS/USB-ről: .hex / .elf / .bin (közös avr_image parser).
 *
 * FIGYELEM: HW-n MÉG NEM IGAZOLT. Lásd reference/AVR_PDI.md.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Detektált PDI (XMEGA) cél leírója. */
typedef struct {
    uint8_t     sig[3];     /* signature (MCU.DEVID), pl. ATxmega32A4: 1E 95 41 */
    const char *name;       /* eszköznév vagy NULL ha ismeretlen */
    uint32_t    flash_size; /* (app) flash méret bájtban */
    uint16_t    page_size;  /* flash lapméret bájtban */
    bool        known;      /* true, ha a signature szerepel a táblában */
} avr_pdi_dev_t;

/* Bit-bang PDI PHY felhozása (Kconfig-lábak). Idempotens. */
esp_err_t avr_pdi_init(void);

/* PDI engedélyezés + signature kiolvasás -> kilépés. out kitöltve. */
esp_err_t avr_pdi_detect(avr_pdi_dev_t *out);

/* Fázis/százalék callback (a flashelő taszk kontextusából hívódik). */
typedef void (*avr_pdi_progress_cb)(const char *phase, int percent, void *ctx);

/* Forrásfájl flashelése PDI-n. A kiterjesztés dönt (.hex/.elf/.bin).
   Folyamat: enable -> NVM key -> erase -> page program -> verify -> reset. */
esp_err_t avr_pdi_flash_file(const char *path, avr_pdi_progress_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif
