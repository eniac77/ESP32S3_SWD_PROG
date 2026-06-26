#pragma once
/* LittleFS forrás-tár: mount + fájl-API + közös lock. (terv 11. szekció)
 *
 * Minden fájlművelet (web_ui, ftp_srv, prog_session) ezen az API-n és a
 * közös lockon keresztül menjen — a LittleFS nem feltétel nélkül thread-safe.
 */
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_LFS_BASE "/lfs"   /* mount pont */

/* Mountolja a "storage" partícióra /lfs alá, létrehozza a
   /lfs/fw, /lfs/cfg, /lfs/www könyvtárakat, inicializálja a lockot. */
esp_err_t storage_lfs_init(void);

/* Közös zár — minden alábbi segéd ezt használja; külső, összetett
   műveletekhez (pl. streamelt feltöltés) is ezt kell fogni/elengedni. */
void storage_lfs_lock(void);
void storage_lfs_unlock(void);

/* Teljes fájl beolvasása (malloc, lehetőleg PSRAM-ba; a hívó free-zi *buf-ot). */
esp_err_t storage_lfs_read_all(const char *path, void **buf, size_t *len);

/* Teljes fájl kiírása (felülír). */
esp_err_t storage_lfs_write_all(const char *path, const void *buf, size_t len);

/* Könyvtár-listázás callbackkel (a lock alatt hívódik). */
typedef void (*storage_lfs_list_cb)(const char *name, size_t size, bool is_dir, void *ctx);
esp_err_t storage_lfs_list(const char *dir, storage_lfs_list_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif
