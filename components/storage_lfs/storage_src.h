#pragma once
/* Forrás-feloldó réteg: a logikai mappát (fw/cfg) az AKTÍV forrás abszolút
 * útvonalára képezi. Ha van mountolt USB stick, az az aktív forrás ("/usb"),
 * különben a belső LittleFS ("/lfs").
 *
 * A list/read/write a path PREFIXE szerint a megfelelő backendet (és annak
 * saját lockját) választja, így a hívóknak nem kell tudniuk a forrásról.
 * A www (web-asszetek) és az FTP szándékosan NEM ezen megy — azok mindig a
 * belső LittleFS-en maradnak (lásd web_ui / ftp_srv).
 */
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "storage_lfs.h"   /* STORAGE_LFS_BASE + storage_lfs_list_cb */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STORAGE_SRC_LFS = 0,   /* belső LittleFS */
    STORAGE_SRC_USB = 1,   /* mountolt USB stick */
} storage_src_t;

/* USB host alréteg indítása (storage_lfs_init UTÁN hívandó). A LittleFS-t a
   storage_lfs_init mountolja; ez csak a (opcionális) USB-ágat hozza fel. */
esp_err_t storage_src_init(void);

/* Az aktuálisan aktív forrás (USB ha van stick, különben LFS). */
storage_src_t storage_src_active(void);

/* Az aktív forrás gyökere: "/usb" vagy STORAGE_LFS_BASE ("/lfs"). */
const char *storage_src_base(void);

/* Rövid, UI-ban megjeleníthető forrásnév ("USB" / "belso"). */
const char *storage_src_name(void);

/* Lock/unlock a path prefixe szerint (a megfelelő backend mutexe). */
void storage_src_lock(const char *path);
void storage_src_unlock(const char *path);

/* Fájl-API: a path prefixe alapján a megfelelő backendre delegál. */
esp_err_t storage_src_read_all(const char *path, void **buf, size_t *len);
esp_err_t storage_src_write_all(const char *path, const void *buf, size_t len);
esp_err_t storage_src_list(const char *dir, storage_lfs_list_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif
