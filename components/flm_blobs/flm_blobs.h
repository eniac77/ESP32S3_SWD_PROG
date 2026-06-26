#pragma once
/* Generált FLM C tömbök + eszköz-leírók (a flm_extract.py tölti).
 *
 * A .FLM (ARM ELF) PrgCode/PrgData/DevDsc szekcióiból generált adat.
 * Ez a típus a flm_runner és a target_db közös szerződése. (terv 8–9.)
 */
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CMSIS FlashDevice szektor-leíró: méret + kezdőcím (az eszköz elejéhez képest). */
typedef struct {
    uint32_t size;
    uint32_t addr;
} flm_sector_t;

/* Egy FLM algoritmus + eszköz-leíró. A prg_code a PrgCode (a PrgData-t a
   loader a kód mögé tölti; az off_* offsetek a prg_code elejéhez képest). */
typedef struct {
    const char         *name;             /* pl. "STM32F4xx 1MB" */
    uint32_t            dev_addr;         /* flash bázis, pl. 0x08000000 */
    uint32_t            dev_size;         /* teljes flash méret */
    uint32_t            page_size;        /* ProgramPage granularitás */
    uint8_t             erased_val;       /* törölt érték, tipikusan 0xFF */
    uint32_t            timeout_prog_ms;
    uint32_t            timeout_erase_ms;
    const flm_sector_t *sectors;          /* szektor-tömb */
    uint32_t            sector_count;
    const uint8_t      *prg_code;         /* PrgCode bájtok */
    uint32_t            prg_code_len;
    uint32_t            prg_data_len;     /* PrgData (RW/ZI) mérete a kód mögött */
    /* ABI belépési pontok a prg_code elejéhez képesti offsetként (0 = nincs): */
    uint32_t off_init, off_uninit, off_erase_chip, off_erase_sector,
             off_program_page, off_verify;
} flm_algo_t;

esp_err_t flm_blobs_init(void);

/* A beépített FLM algoritmusok táblája. *count megkapja az elemszámot.
   Lehet üres (count=0), amíg nincs vendored .FLM. */
const flm_algo_t * const *flm_blobs_table(size_t *count);

#ifdef __cplusplus
}
#endif
