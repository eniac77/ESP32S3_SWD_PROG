#pragma once
/* Helyi UI: az enkóder-eseményeket fogyasztja és ILI9488 LCD-re renderel (LVGL).
 * A display_lcd + input_enc fölött ül (terv 15. szekció; LVGL-port:
 * reference/ILI9488_LVGL_port.md).
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Létrehozza a ui_task-ot (feltételezi: display_lcd_init + input_enc_init
   már megtörtént). A taszk a saját eseménysorán pörög, csak változásra renderel. */
esp_err_t ui_start(void);

#ifdef __cplusplus
}
#endif
