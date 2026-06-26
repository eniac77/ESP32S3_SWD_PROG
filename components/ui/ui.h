#pragma once
/* Helyi UI: az enkóder-eseményeket fogyasztja és OLED-re renderel.
 * A display_oled + input_enc fölött ül (terv 15. szekció).
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Létrehozza a ui_task-ot (feltételezi: display_oled_init + input_enc_init
   már megtörtént). A taszk a saját eseménysorán pörög, csak változásra renderel. */
esp_err_t ui_start(void);

#ifdef __cplusplus
}
#endif
