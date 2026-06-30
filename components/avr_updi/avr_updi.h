#pragma once
/* AVR UPDI programozó (modern tinyAVR/megaAVR 0/1/2-series, AVR Dx).
 *
 * Az UPDI (Unified Program and Debug Interface) EGYETLEN vezetékes, UART-alapú
 * (8E2, LSB-first, half-duplex) interfész. A meglévő "pines" programozó-headeren
 * a UPDI-vonalat a MISO-lábra kötjük (lásd Kconfig: CONFIG_AVR_UPDI_GPIO,
 * default GPIO7) — a target UPDI/RESET lába ide megy, soros ~4,7k ajánlott.
 *
 * Teljesen független az SPI-ISP (avr_isp) és a SWD/FLM magtól. A flashelés az
 * UPDI link-rétegen (BREAK/SYNC + LDS/STS/LDCS/STCS/KEY) és az NVM-vezérlőn át
 * megy. Forrás LittleFS/USB-ről: .hex / .elf / .bin (közös avr_image parser).
 *
 * FIGYELEM: HW-n MÉG NEM IGAZOLT (nincs validáló cél a padon). A link-réteg a
 * dokumentált UPDI ABI szerint készült; az NVMCTRL-folyam a tinyAVR/megaAVR
 * 0/1/2 (NVMCTRL v0) családra van bekötve. Bring-up előtt lásd a reference doksit.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Detektált UPDI cél leírója. */
typedef struct {
    uint8_t     sig[3];     /* signature bájtok (SIGROW), pl. ATtiny412: 1E 92 23 */
    const char *name;       /* eszköznév vagy NULL ha ismeretlen */
    uint32_t    flash_size; /* flash méret bájtban */
    uint16_t    page_size;  /* flash lapméret bájtban (NVM page-buffer granularitás) */
    bool        known;      /* true, ha a signature szerepel a táblában */
} avr_updi_dev_t;

/* UART single-wire UPDI PHY felhozása (Kconfig-láb). Idempotens. */
esp_err_t avr_updi_init(void);

/* UPDI engedélyezés + signature kiolvasás -> kilépés. out kitöltve.
   Ismeretlen signature esetén known=false, name=NULL (a sig akkor is megvan). */
esp_err_t avr_updi_detect(avr_updi_dev_t *out);

/* Fázis/százalék callback (a flashelő taszk kontextusában hívódik). */
typedef void (*avr_updi_progress_cb)(const char *phase, int percent, void *ctx);

/* Forrásfájl flashelése UPDI-n. A kiterjesztés dönt (.hex/.elf/.bin).
   Folyamat: enable -> NVMPROG key -> reset prog-módba -> (chip)erase ->
   page program -> verify -> reset alkalmazásba. cb lehet NULL. */
esp_err_t avr_updi_flash_file(const char *path, avr_updi_progress_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif
