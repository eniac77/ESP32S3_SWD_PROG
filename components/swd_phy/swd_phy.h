#pragma once
/* SWD fizikai réteg (dedic_gpio HAL): seq_out/seq_in/dir/idle — platformspecifikus */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Komponens inicializálás (skeleton stub). */
esp_err_t swd_phy_init(void);

#ifdef __cplusplus
}
#endif
