#pragma once
/* Minimális FTP szerver a LittleFS (/lfs) fölött. (terv 12.3)
 *
 * Passzív mód, plaintext (csak LAN), 1–2 session. A storage_lfs lockját
 * használja (ne ütközzön a web_ui-val). Parancsok: USER/PASS/SYST/PWD/CWD/
 * CDUP/TYPE/PASV/LIST/NLST/RETR/STOR/DELE/SIZE/QUIT.
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Elindítja az FTP control-listener taszkot (port 21). */
esp_err_t ftp_srv_init(void);

#ifdef __cplusplus
}
#endif
