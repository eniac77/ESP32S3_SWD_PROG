#pragma once
/* LittleFS mount + fájl-API + közös lock (FONTOS: a tényleges littlefs függőséget KÉSŐBB adjuk hozzá, most csak stub) */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Komponens inicializálás (skeleton stub). */
esp_err_t storage_lfs_init(void);

#ifdef __cplusplus
}
#endif
