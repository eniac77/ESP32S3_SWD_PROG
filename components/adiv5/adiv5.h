#pragma once
/* ADIv5 transport: DP/AP, JTAG-to-SWD switch, ACK/parity, AHB-AP mem R/W.
 *
 * A swd_phy.h primitíveire épül; családfüggetlen. (terv 6. szekció)
 */
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t adiv5_init(void);

/* Teljes bring-up: ≥50 SWCLK (SWDIO=1) -> JTAG-to-SWD switch (0xE79E) ->
   ≥50 SWCLK -> DPIDR olvasás -> debug power-up (CDBGPWRUPREQ|CSYSPWRUPREQ)
   -> AHB-AP alap CSW (Word + AddrInc Single). idcode_out NULL megengedett. */
esp_err_t adiv5_connect(uint32_t *idcode_out);

/* Memória hozzáférés AHB-AP-n keresztül (block: TAR auto-increment). */
esp_err_t adiv5_read32(uint32_t addr, uint32_t *val);
esp_err_t adiv5_write32(uint32_t addr, uint32_t val);
esp_err_t adiv5_read_block(uint32_t addr, uint32_t *buf, size_t words);
esp_err_t adiv5_write_block(uint32_t addr, const uint32_t *buf, size_t words);

#ifdef __cplusplus
}
#endif
