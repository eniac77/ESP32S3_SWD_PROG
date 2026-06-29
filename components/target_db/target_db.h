#pragma once
/* Cél-adatbázis: DBGMCU IDCODE / DEV_ID -> család + flash-size reg + FLM.
 * STM32 F0/F1/F2/F3/F4/F7/H7/H5/G0/G4/L0/L1/L4/L5/U0/U5/WB/WL/WBA/C0.
 * (terv 10. szekció; a teljes 80-DEV_ID paletta a STM32CubeProgrammer
 * FlashLoader készletéhez igazítva.)
 */
#include <stdint.h>
#include "esp_err.h"
#include "flm_blobs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STM32_FAM_F0, STM32_FAM_F1, STM32_FAM_F2, STM32_FAM_F3, STM32_FAM_F4,
    STM32_FAM_F7, STM32_FAM_H7, STM32_FAM_H5,
    STM32_FAM_G0, STM32_FAM_G4,
    STM32_FAM_L0, STM32_FAM_L1, STM32_FAM_L4, STM32_FAM_L5,
    STM32_FAM_U0, STM32_FAM_U5,
    STM32_FAM_WB, STM32_FAM_WL, STM32_FAM_WBA,
    STM32_FAM_C0,
    STM32_FAM_UNKNOWN
} stm32_family_t;

/* RDP (Readout Protection) regiszter típusa az értelmezéshez. A regiszter
   címe és kódolása családspecifikus; RDP_REG_NONE = nem ismert/nem olvassuk. */
typedef enum {
    RDP_REG_NONE = 0,
    RDP_REG_OBR_F1,    /* FLASH_OBR: RDPRT bit -> level1 (F0/F1/F3) */
    RDP_REG_OPTCR_F4,  /* FLASH_OPTCR: RDP byte [15:8] (0xAA=L0, 0xCC=L2) (F4/F7) */
    RDP_REG_OPTR_L4,   /* FLASH_OPTR: RDP byte [7:0] (0xAA=L0, 0xCC=L2) (L4/G0/L0) */
} rdp_reg_kind_t;

typedef struct {
    stm32_family_t family;
    const char    *name;              /* pl. "STM32F40x/F41x" */
    uint16_t       dev_id;            /* DBGMCU IDCODE alsó 12 bit (DEV_ID) */
    uint32_t       dbgmcu_idcode_addr;/* IDCODE regiszter címe (0xE0042000 v. 0x40015800) */
    uint32_t       flash_size_addr;   /* F-size regiszter címe (családspecifikus) */
    uint32_t       flash_base;        /* tipikusan 0x08000000 */
    const char    *prog_note;         /* pl. "double-word", "half-page" */
    uint32_t       rdp_addr;          /* RDP/option regiszter címe (0 = nincs/ismeretlen) */
    rdp_reg_kind_t rdp_kind;          /* a rdp_addr értelmezése */
} target_info_t;

esp_err_t target_db_init(void);

/* DEV_ID (12 bit) alapján a leíró, vagy NULL ha ismeretlen. */
const target_info_t *target_db_lookup(uint16_t dev_id);

/* IDCODE regiszter lehetséges címei (a hívó ezeket próbálja végig SWD-n,
   amíg értelmes DEV_ID-t kap). NULL-terminált lista, *n az elemszám. */
const uint32_t *target_db_idcode_addrs(size_t *n);

/* A célhoz illő FLM algoritmus kiválasztása a flm_blobs táblából
   (család + tényleges flash méret alapján). NULL ha nincs találat. */
const flm_algo_t *target_db_select_flm(const target_info_t *info, uint32_t flash_size);

/* RDP szint a leíró rdp_kind-ja + a kiolvasott regiszter-érték alapján.
   Visszaad: 0 (védtelen) / 1 / 2, vagy -1 ha nem értelmezhető (RDP_REG_NONE). */
int target_db_rdp_level(const target_info_t *info, uint32_t reg_value);

#ifdef __cplusplus
}
#endif
