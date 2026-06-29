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

/* A SWDIO pad pillanatnyi bemeneti szintje (0/1) — diagnosztikához.
   A hívó előbb tegyen turnaroundot bemenetre (swd_phy_dir(false)), ha a
   cél/szabad vonal szintjére kíváncsi. Nem órajelez, csak mintát vesz. */
int      swd_phy_read_level(void);

/* --- Diagnosztika (bench bring-up) ---
 * PHY-szintű IO-önteszt: ellenőrzi, hogy a bemeneti út (dedic input bundle)
 * valóban a SWDIO pad fizikai szintjét olvassa-e. Loopback a padon át:
 *   1) released (magas-Z, csak pullup) -> elvárt olvasat 1
 *   2) meghajtva 0 -> elvárt olvasat 0   (ha 1, a read-út hibás -> csupa-1 ACK!)
 *   3) meghajtva 1 -> elvárt olvasat 1
 * Plusz néhány látható SWCLK pulzus analizátorhoz. A vizsgálat után a PHY-t
 * biztonságos alapba állítja (SWDIO meghajtva magasra, SWCLK alacsony).
 * Visszaad: true, ha mindhárom olvasat az elvárt -> a PHY read-út jó. */
bool     swd_phy_selftest_io(void);

#ifdef __cplusplus
}
#endif
