#pragma once
/* Cél-adatbázis: DBGMCU IDCODE / DEV_ID -> család + flash-size reg + FLM.
 * STM32 F0/F1/F3/F4/F7/L0/L1/L4/G0. (terv 10. szekció)
 */
#include <stdint.h>
#include "esp_err.h"
#include "flm_blobs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STM32_FAM_F0, STM32_FAM_F1, STM32_FAM_F3, STM32_FAM_F4, STM32_FAM_F7,
    STM32_FAM_L0, STM32_FAM_L1, STM32_FAM_L4, STM32_FAM_G0,
    STM32_FAM_UNKNOWN
} stm32_family_t;

typedef struct {
    stm32_family_t family;
    const char    *name;              /* pl. "STM32F40x/F41x" */
    uint16_t       dev_id;            /* DBGMCU IDCODE alsó 12 bit (DEV_ID) */
    uint32_t       dbgmcu_idcode_addr;/* IDCODE regiszter címe (0xE0042000 v. 0x40015800) */
    uint32_t       flash_size_addr;   /* F-size regiszter címe (családspecifikus) */
    uint32_t       flash_base;        /* tipikusan 0x08000000 */
    const char    *prog_note;         /* pl. "double-word", "half-page" */
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

#ifdef __cplusplus
}
#endif
