#pragma once
/* esp_http_server: statikus UI (/lfs/www) + REST API + WebSocket. (terv 12.2)
 *
 * REST: /api/files, /api/upload, /api/download, /api/file (DELETE),
 *       /api/program, /api/cfg/push, /api/cfg/pull.
 * WS /ws: élő adat (target_state) + programozási progress + log.
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Elindítja a HTTP szervert és regisztrálja a handlereket.
   (A storage_lfs és target_state már inicializált; a WiFi indulhat utána is.) */
esp_err_t web_ui_init(void);

/* Üzenet broadcast minden WS klienshez (pl. progress/log JSON). */
esp_err_t web_ui_ws_broadcast(const char *json);

#ifdef __cplusplus
}
#endif
