#pragma once
/* USB MSC (pendrive) forrás-tár: USB host + MSC class + FATFS mount /usb alá.
 *
 * Opcionális réteg (CONFIG_USB_MSC_HOST_ENABLE). Kikapcsolva minden függvény
 * no-op / "nincs stick", így a build és a viselkedés változatlan marad — a
 * natív USB-Serial/JTAG flash/monitor megmarad. Bekapcsolva a GPIO19/20 native
 * USB pad OTG host módba kerül (a másodlagos USB-Serial/JTAG konzolt KI kell
 * kapcsolni — közös USB-PHY; lásd a fordítási #error-t a .c-ben).
 *
 * A fájl-API szemantikája megegyezik a storage_lfs-szel; az utak /usb-abszolútak
 * (pl. "/usb/fw/app.bin"). A forrásválasztást a storage_src réteg végzi.
 */
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_USB_BASE "/usb"   /* FATFS mount pont */

/* USB host + MSC class driver indítása. Kikapcsolva no-op (ESP_OK).
   A tényleges mount aszinkron, stick bedugásakor történik (hot-plug). */
esp_err_t storage_usb_init(void);

/* Van-e épp mountolt (használható) USB stick. Kikapcsolva mindig false. */
bool storage_usb_present(void);

/* Saját recursive lock (a FATFS sem feltétel nélkül thread-safe). */
void storage_usb_lock(void);
void storage_usb_unlock(void);

/* Teljes fájl beolvasása (malloc, lehetőleg PSRAM; a hívó free-zi *buf-ot). */
esp_err_t storage_usb_read_all(const char *path, void **buf, size_t *len);

/* Teljes fájl kiírása (felülír). */
esp_err_t storage_usb_write_all(const char *path, const void *buf, size_t len);

/* Könyvtár-listázás callbackkel (a lock alatt hívódik). */
typedef void (*storage_usb_list_cb)(const char *name, size_t size, bool is_dir, void *ctx);
esp_err_t storage_usb_list(const char *dir, storage_usb_list_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif
