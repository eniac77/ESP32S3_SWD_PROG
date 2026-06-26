#pragma once
/* SWD programozás orchestráció: connect-under-reset -> erase -> program -> verify, progress callback */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Komponens inicializálás (skeleton stub). */
esp_err_t prog_session_init(void);

#ifdef __cplusplus
}
#endif
