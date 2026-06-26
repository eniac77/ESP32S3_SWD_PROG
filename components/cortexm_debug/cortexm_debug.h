#pragma once
/* Cortex-M debug: halt/resume/reset, connect-under-reset, core reg R/W.
 *
 * Az adiv5.h mem R/W primitíveire épül; családfüggetlen. (terv 7. szekció)
 */
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DCRSR REGSEL: R0..R12=0..12, SP=13, LR=14, PC=15, xPSR=16. */
enum {
    CM_REG_R0   = 0,
    CM_REG_SP   = 13,
    CM_REG_LR   = 14,
    CM_REG_PC   = 15,
    CM_REG_XPSR = 16,
};

esp_err_t cortexm_debug_init(void);

esp_err_t cortexm_halt(void);        /* DHCSR 0xA05F0003, poll S_HALT */
esp_err_t cortexm_resume(void);      /* DHCSR 0xA05F0001 */
esp_err_t cortexm_sysreset(void);    /* AIRCR 0x05FA0004 (SYSRESETREQ) */

/* Connect-under-reset (KÖTELEZŐ): nRST assert -> adiv5_connect ->
   DEMCR VC_CORERESET -> nRST release -> halt a reset-vektoron.
   idcode_out NULL megengedett. */
esp_err_t cortexm_connect_under_reset(uint32_t *idcode_out);

esp_err_t cortexm_reg_read(int regsel, uint32_t *val);
esp_err_t cortexm_reg_write(int regsel, uint32_t val);

/* Várakozás halt állapotra (DHCSR S_HALT) timeout_ms-ig. */
esp_err_t cortexm_wait_halt(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
