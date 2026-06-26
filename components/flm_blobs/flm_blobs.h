#pragma once
/* Flash-loader leírók + RAM-képek (a flm_extract.py generálja).
 *
 * KÉT ABI támogatott (terv 8–9. + eltérés-jegyzet a CLAUDE.md-ben):
 *   - FLM_ABI_CMSIS: klasszikus CMSIS-Pack .FLM (PrgCode/DevDsc, 0=siker,
 *     Init/EraseSector/ProgramPage/Verify, az off_* a kód elejéhez RELATÍV).
 *   - FLM_ABI_ST: STM32CubeProgrammer .stldr (StorageInfo, 1=siker,
 *     Init/Write/SectorErase/MassErase/Verify; a code-ot a load_addr ABSZOLÚT
 *     címére kell tölteni, az off_* a szimbólumok ABSZOLÚT címei).
 */
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t size;   /* szektor mérete */
    uint32_t addr;   /* kezdőcím az eszköz elejéhez képest */
} flm_sector_t;

typedef enum {
    FLM_ABI_CMSIS = 0,
    FLM_ABI_ST    = 1,
} flm_abi_t;

typedef struct {
    const char         *name;             /* eszköznév */
    uint16_t            dev_id;           /* DBGMCU DEV_ID (ST .stldr fájlnévből); 0 = n/a */
    flm_abi_t           abi;
    int                 success_ret;      /* sikeres visszatérési érték (CMSIS:0, ST:1) */

    uint32_t            dev_addr;         /* flash bázis (pl. 0x08000000) */
    uint32_t            dev_size;
    uint32_t            page_size;        /* programozási granularitás */
    uint8_t             erased_val;       /* törölt érték (tipikusan 0xFF) */
    uint32_t            timeout_prog_ms;
    uint32_t            timeout_erase_ms;
    const flm_sector_t *sectors;          /* informatív; ST-nél erase tartomány alapján megy */
    uint32_t            sector_count;

    /* A loader RAM-képe: a `code`-ot a `load_addr` címre kell tölteni a cél
       RAM-jába. CMSIS: load_addr=0x20000000. ST: a section abszolút címe. */
    uint32_t            load_addr;
    const uint8_t      *code;
    uint32_t            code_len;
    uint32_t            data_len;         /* CMSIS: PrgData a code mögött; ST: 0 */

    /* Belépési pontok.
       CMSIS ABI: a `load_addr`-hoz RELATÍV offset.
       ST ABI: ABSZOLÚT cím (Thumb-bit nélkül). 0 = nincs. */
    uint32_t off_init;            /* CMSIS: Init(adr,clk,fnc); ST: Init(void) */
    uint32_t off_uninit;          /* CMSIS */
    uint32_t off_erase_chip;      /* CMSIS EraseChip */
    uint32_t off_erase_sector;    /* CMSIS EraseSector(adr) */
    uint32_t off_program_page;    /* CMSIS ProgramPage(adr,sz,buf) */
    uint32_t off_verify;          /* CMSIS/ST Verify */
    uint32_t off_st_write;        /* ST Write(addr,size,buf) */
    uint32_t off_st_sector_erase; /* ST SectorErase(start,end) */
    uint32_t off_st_mass_erase;   /* ST MassErase(void) */
} flm_algo_t;

esp_err_t flm_blobs_init(void);

/* A beépített flash-loaderek táblája. *count megkapja az elemszámot.
   Lehet üres (count=0), ha nincs generált tábla. */
const flm_algo_t * const *flm_blobs_table(size_t *count);

#ifdef __cplusplus
}
#endif
