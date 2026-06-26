#pragma once
/* SWD fizikai réteg (dedic_gpio HAL) — platformspecifikus.
 *
 * Ez az EGYETLEN platformfüggő rétege a SWD/FLM magnak. A felette lévő
 * adiv5/cortexm_debug kizárólag az itt deklarált primitíveket hívja.
 * Lábak (terv 2.2): SWCLK=GPIO4, SWDIO=GPIO5, nRST=GPIO6 (open-drain + pullup).
 */
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* dedic_gpio bundle-ök + lábak inicializálása. */
esp_err_t swd_phy_init(void);

/* --- SWD bit-szintű primitívek (a mag ezt hívja) --- */
void     swd_phy_seq_out(uint32_t bits, int n);  /* n bit kiküldése, LSB-first */
uint32_t swd_phy_seq_in(int n);                  /* n bit beolvasása, LSB-first */
void     swd_phy_dir(bool drive);                /* SWDIO irány: true=meghajt, false=bemenet (turnaround) */
void     swd_phy_idle(int clocks);               /* idle/line-reset: SWDIO=1 tartva `clocks` órajelig */

/* SWCLK közelítő frekvencia (best-effort; 0 = leggyorsabb elérhető).
   Bring-up alatt ~200–500 kHz ajánlott, utána feltolható. */
void     swd_phy_set_freq_hz(uint32_t hz);

/* nRST (open-drain) vezérlés: assert=true -> reset aktív (alacsony szint). */
void     swd_phy_nrst(bool assert);

#ifdef __cplusplus
}
#endif
