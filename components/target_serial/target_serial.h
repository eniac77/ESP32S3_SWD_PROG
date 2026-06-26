#pragma once
/* UART híd a futó cél STM32-höz (3.3V): keretezett protokoll, .cfg fel/le, élő adat */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Komponens inicializálás (skeleton stub). */
esp_err_t target_serial_init(void);

#ifdef __cplusplus
}
#endif
