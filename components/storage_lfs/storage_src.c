/* Forrás-feloldó réteg — lásd storage_src.h.
 *
 * A diszpécs egyszerű: ha a path a "/usb" prefixszel kezdődik, az USB backendre
 * megy (storage_usb_*), különben a belső LittleFS-re (storage_lfs_*). Mindkét
 * backend a saját, recursive lockja alatt dolgozik.
 */
#include "storage_src.h"
#include "storage_usb.h"

#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"

static const char *TAG = "storage_src";

/* Igaz, ha a path a /usb mount alá esik. */
static bool is_usb_path(const char *p)
{
    if (p == NULL) return false;
    size_t bl = strlen(STORAGE_USB_BASE);
    return strncmp(p, STORAGE_USB_BASE, bl) == 0;
}

esp_err_t storage_src_init(void)
{
#if CONFIG_USB_MSC_HOST_ENABLE
    ESP_LOGI(TAG, "USB MSC forras BEKAPCSOLVA — host indul (mount: %s)", STORAGE_USB_BASE);
#else
    ESP_LOGI(TAG, "USB MSC forras KIKAPCSOLVA (CONFIG_USB_MSC_HOST_ENABLE=n) "
                  "— csak belso LittleFS (/lfs)");
#endif
    return storage_usb_init();   /* kikapcsolva no-op */
}

storage_src_t storage_src_active(void)
{
    return storage_usb_present() ? STORAGE_SRC_USB : STORAGE_SRC_LFS;
}

const char *storage_src_base(void)
{
    return storage_usb_present() ? STORAGE_USB_BASE : STORAGE_LFS_BASE;
}

const char *storage_src_name(void)
{
    return storage_usb_present() ? "USB" : "belso";
}

void storage_src_lock(const char *path)
{
    if (is_usb_path(path)) storage_usb_lock();
    else                   storage_lfs_lock();
}

void storage_src_unlock(const char *path)
{
    if (is_usb_path(path)) storage_usb_unlock();
    else                   storage_lfs_unlock();
}

esp_err_t storage_src_read_all(const char *path, void **buf, size_t *len)
{
    return is_usb_path(path) ? storage_usb_read_all(path, buf, len)
                             : storage_lfs_read_all(path, buf, len);
}

esp_err_t storage_src_write_all(const char *path, const void *buf, size_t len)
{
    return is_usb_path(path) ? storage_usb_write_all(path, buf, len)
                             : storage_lfs_write_all(path, buf, len);
}

esp_err_t storage_src_list(const char *dir, storage_lfs_list_cb cb, void *ctx)
{
    /* A két cb-típus szignatúrája azonos -> kompatibilis. */
    return is_usb_path(dir) ? storage_usb_list(dir, (storage_usb_list_cb)cb, ctx)
                            : storage_lfs_list(dir, cb, ctx);
}
