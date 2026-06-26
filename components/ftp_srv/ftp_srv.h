#pragma once
/* FTP szerver a LittleFS fölött (fw/cfg fel-/letöltés, storage lock) */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Komponens inicializálás (skeleton stub). */
esp_err_t ftp_srv_init(void);

#ifdef __cplusplus
}
#endif
