#pragma once
/* CMSIS FLM futtató: call_function a cél RAM-jában (Init/Erase/Program/Verify) */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Komponens inicializálás (skeleton stub). */
esp_err_t flm_runner_init(void);

#ifdef __cplusplus
}
#endif
