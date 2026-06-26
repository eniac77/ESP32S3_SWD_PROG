#pragma once
/* CMSIS FLM futtató: PrgCode betöltése a cél RAM-jába + call_function
 * (Init/Erase/Program/Verify), teljes flash orchestráció. (terv 8. szekció)
 *
 * A cortexm_debug (reg R/W, halt/resume) és adiv5 (mem R/W) rétegre épül,
 * a flm_algo_t leírót a flm_blobs/target_db adja.
 */
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "flm_blobs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Progress callback: fázis ("Erase"/"Program"/"Verify"), kész/összes byte. */
typedef void (*flm_progress_cb)(const char *phase, uint32_t done, uint32_t total, void *ctx);

esp_err_t flm_runner_init(void);

/* PrgCode(+PrgData) betöltése a cél RAM-jába (0x20000000-tól), stack/buffer
   elrendezés kiszámítása. A célnak haltolt állapotban kell lennie. */
esp_err_t flm_runner_load(const flm_algo_t *algo);

/* Egy ABI függvény meghívása (call_function): R0..R3 paraméter, a visszatérési
   R0 a *ret-ben. func_off a prg_code elejéhez képesti offset. */
esp_err_t flm_runner_call(uint32_t func_off,
                          uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
                          uint32_t timeout_ms, int *ret);

/* Magas szintű: a betöltött algoritmussal erase + program + verify a
   `data`/`len` tartalommal `base_addr`-tól. A cél haltolt; a hívó intézi
   a connect-under-reset-et és a végén a reset&run-t. */
esp_err_t flm_runner_program(const flm_algo_t *algo, uint32_t base_addr,
                             const uint8_t *data, size_t len,
                             flm_progress_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif
