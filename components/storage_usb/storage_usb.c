/* USB MSC (pendrive) forrás-tár megvalósítás.
 *
 * CONFIG_USB_MSC_HOST_ENABLE nélkül az egész fájl stub (no-op + "nincs stick"),
 * így nulla a viselkedésváltozás és nem kell USB-host kódot fordítani.
 *
 * Bekapcsolva: usb_host_install + MSC class driver (saját háttér-taszkkal) +
 * usb_host_lib_handle_events daemon-taszk. Stick bedugásakor (MSC_DEVICE_
 * CONNECTED) install_device + FATFS mount /usb alá; kihúzáskor (MSC_DEVICE_
 * DISCONNECTED) vfs_unregister + uninstall_device. A fájl-API a szokásos POSIX
 * (fopen/opendir/stat) a /usb VFS fölött, saját recursive mutex alatt.
 */
#include "storage_usb.h"
#include "sdkconfig.h"

#if !CONFIG_USB_MSC_HOST_ENABLE
/* ====================================================================== */
/* Kikapcsolt változat — stub                                             */
/* ====================================================================== */
esp_err_t storage_usb_init(void) { return ESP_OK; }
bool      storage_usb_present(void) { return false; }
void      storage_usb_lock(void) {}
void      storage_usb_unlock(void) {}

esp_err_t storage_usb_read_all(const char *path, void **buf, size_t *len)
{
    (void)path;
    if (buf) *buf = NULL;
    if (len) *len = 0;
    return ESP_ERR_NOT_FOUND;
}
esp_err_t storage_usb_write_all(const char *path, const void *buf, size_t len)
{
    (void)path; (void)buf; (void)len;
    return ESP_ERR_NOT_FOUND;
}
esp_err_t storage_usb_list(const char *dir, storage_usb_list_cb cb, void *ctx)
{
    (void)dir; (void)cb; (void)ctx;
    return ESP_ERR_NOT_FOUND;
}

#else  /* CONFIG_USB_MSC_HOST_ENABLE */
/* ====================================================================== */
/* Bekapcsolt változat — USB host + MSC + FATFS                           */
/* ====================================================================== */

/* A native USB pad (GPIO19/20) megosztott: OTG host módban a másodlagos
   USB-Serial/JTAG konzol nem használható. Kapcsold ki az sdkconfig-ban
   (CONFIG_ESP_CONSOLE_SECONDARY_NONE=y). Az elsődleges konzol UART0 marad. */
#if CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
#error "USB MSC host (CONFIG_USB_MSC_HOST_ENABLE) utkozik a masodlagos USB-Serial/JTAG konzollal (kozos USB-PHY). Allitsd CONFIG_ESP_CONSOLE_SECONDARY_NONE=y-re (konzol UART0-n)."
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"

static const char *TAG = "storage_usb";

static SemaphoreHandle_t       s_mutex   = NULL;
static volatile bool           s_mounted = false;
static msc_host_device_handle_t s_dev    = NULL;
static msc_host_vfs_handle_t    s_vfs    = NULL;
static QueueHandle_t           s_evt_q   = NULL;  /* MSC esemenyek a callbacktol */

/* ---- USB host library daemon-taszk: a host-lib eseményeit pörgeti. ---- */
static void usb_lib_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
        /* ALL_FREE-t nem kezelünk: a host-ot nem állítjuk le futás közben. */
    }
}

/* ---- Mount / unmount (a MSC háttér-taszk kontextusából hívva). ---- */
static void usb_mount(uint8_t addr)
{
    storage_usb_lock();
    esp_err_t e = msc_host_install_device(addr, &s_dev);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_install_device(addr=%u): %s", addr, esp_err_to_name(e));
        s_dev = NULL;
        storage_usb_unlock();
        return;
    }

    /* A stick FAT-ját NE formázzuk hiba esetén — a felhasználó adata. */
    const esp_vfs_fat_mount_config_t mc = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 0,
    };
    e = msc_host_vfs_register(s_dev, STORAGE_USB_BASE, &mc, &s_vfs);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_vfs_register('%s'): %s — FAT32 a varhato formatum",
                 STORAGE_USB_BASE, esp_err_to_name(e));
        msc_host_uninstall_device(s_dev);
        s_dev = NULL;
        s_vfs = NULL;
        storage_usb_unlock();
        return;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "USB stick mountolva: %s (forras aktiv)", STORAGE_USB_BASE);
    storage_usb_unlock();
}

static void usb_unmount(void)
{
    storage_usb_lock();
    s_mounted = false;
    if (s_vfs) { msc_host_vfs_unregister(s_vfs); s_vfs = NULL; }
    if (s_dev) { msc_host_uninstall_device(s_dev); s_dev = NULL; }
    ESP_LOGI(TAG, "USB stick eltavolitva — visszaallas a belso tarra");
    storage_usb_unlock();
}

/* ---- MSC eseny-callback (a MSC driver hattertaszkjabol). ----
   FONTOS: a callback a MSC driver hattertaszkjaban fut; innen NEM szabad
   msc_host_install_device-t hivni (deadlock — ugyanaz a taszk pumpalna az
   esemenyeket). Ezert csak sorba tesszuk, a mount/unmount kulon taszkban megy
   (lasd usb_msc_task) — a hivatalos peldaval egyezo minta. */
static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    (void)arg;
    if (s_evt_q) {
        xQueueSend(s_evt_q, event, 0);
    }
}

/* ---- Mount/unmount taszk: a sorbol veszi az esemenyeket. ---- */
static void usb_msc_task(void *arg)
{
    (void)arg;
    msc_host_event_t e;
    while (1) {
        if (xQueueReceive(s_evt_q, &e, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (e.event == MSC_DEVICE_CONNECTED) {
            ESP_LOGI(TAG, "MSC eszkoz csatlakozott (addr=%u)", e.device.address);
            usb_mount(e.device.address);
        } else if (e.event == MSC_DEVICE_DISCONNECTED) {
            ESP_LOGI(TAG, "MSC eszkoz lecsatlakozott");
            usb_unmount();
        }
    }
}

esp_err_t storage_usb_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateRecursiveMutex();
        if (s_mutex == NULL) {
            ESP_LOGE(TAG, "recursive mutex letrehozasa sikertelen");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Opcionalis VBUS load-switch / boost EN lab felhuzasa (5 V a stickhez). */
#if CONFIG_USB_MSC_HOST_VBUS_GPIO >= 0
    {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << CONFIG_USB_MSC_HOST_VBUS_GPIO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        gpio_set_level(CONFIG_USB_MSC_HOST_VBUS_GPIO, 1);
        ESP_LOGI(TAG, "VBUS EN GPIO%d -> magas", CONFIG_USB_MSC_HOST_VBUS_GPIO);
    }
#endif

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t e = usb_host_install(&host_cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(e));
        return e;
    }

    BaseType_t ok = xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "usb_lib taszk letrehozasa sikertelen");
        usb_host_uninstall();
        return ESP_ERR_NO_MEM;
    }

    /* Esemeny-sor + mount/unmount taszk a MSC install_device-hez (a callbackbol
       NEM hivhato kozvetlenul). Letre KELL hozni a msc_host_install elott, hogy
       az elso connect esemenyt mar fogadja. */
    s_evt_q = xQueueCreate(4, sizeof(msc_host_event_t));
    if (s_evt_q == NULL) {
        ESP_LOGE(TAG, "MSC esemeny-sor letrehozasa sikertelen");
        usb_host_uninstall();
        return ESP_ERR_NO_MEM;
    }
    ok = xTaskCreate(usb_msc_task, "usb_msc", 5120, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "usb_msc taszk letrehozasa sikertelen");
        usb_host_uninstall();
        return ESP_ERR_NO_MEM;
    }

    const msc_host_driver_config_t msc_cfg = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .callback = msc_event_cb,
        .callback_arg = NULL,
    };
    e = msc_host_install(&msc_cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_install: %s", esp_err_to_name(e));
        return e;
    }

    ESP_LOGI(TAG, "USB MSC host kesz — pendrive bedugasra var (mount: %s)",
             STORAGE_USB_BASE);
    return ESP_OK;
}

bool storage_usb_present(void) { return s_mounted; }

void storage_usb_lock(void)
{
    if (s_mutex != NULL) xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
}
void storage_usb_unlock(void)
{
    if (s_mutex != NULL) xSemaphoreGiveRecursive(s_mutex);
}

esp_err_t storage_usb_read_all(const char *path, void **buf, size_t *len)
{
    if (path == NULL || buf == NULL || len == NULL) return ESP_ERR_INVALID_ARG;
    *buf = NULL; *len = 0;

    esp_err_t ret = ESP_OK;
    void *data = NULL;
    FILE *f = NULL;

    storage_usb_lock();
    f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen('%s', rb): errno=%d", path, errno);
        ret = ESP_ERR_NOT_FOUND;
        goto out;
    }
    if (fseek(f, 0, SEEK_END) != 0) { ret = ESP_FAIL; goto out; }
    long size = ftell(f);
    if (size < 0) { ret = ESP_FAIL; goto out; }
    rewind(f);

    size_t alloc = (size_t)size + 1;
    data = heap_caps_malloc(alloc, MALLOC_CAP_SPIRAM);
    if (data == NULL) data = malloc(alloc);
    if (data == NULL) { ret = ESP_ERR_NO_MEM; goto out; }

    size_t rd = (size > 0) ? fread(data, 1, (size_t)size, f) : 0;
    if (rd != (size_t)size) {
        ESP_LOGE(TAG, "fread csonka: '%s' (%u/%ld)", path, (unsigned)rd, size);
        free(data); data = NULL; ret = ESP_FAIL; goto out;
    }
    ((uint8_t *)data)[size] = 0;
    *buf = data; *len = (size_t)size; data = NULL;

out:
    if (f) fclose(f);
    storage_usb_unlock();
    return ret;
}

esp_err_t storage_usb_write_all(const char *path, const void *buf, size_t len)
{
    if (path == NULL || (buf == NULL && len > 0)) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    FILE *f = NULL;

    storage_usb_lock();
    f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen('%s', wb): errno=%d", path, errno);
        ret = ESP_FAIL; goto out;
    }
    if (len > 0) {
        size_t wr = fwrite(buf, 1, len, f);
        if (wr != len) {
            ESP_LOGE(TAG, "fwrite csonka: '%s' (%u/%u)", path, (unsigned)wr, (unsigned)len);
            ret = ESP_FAIL; goto out;
        }
    }
out:
    if (f) {
        if (fclose(f) != 0 && ret == ESP_OK) ret = ESP_FAIL;
    }
    storage_usb_unlock();
    return ret;
}

esp_err_t storage_usb_list(const char *dir, storage_usb_list_cb cb, void *ctx)
{
    if (dir == NULL || cb == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    DIR *d = NULL;

    storage_usb_lock();
    d = opendir(dir);
    if (d == NULL) {
        ESP_LOGW(TAG, "opendir('%s'): errno=%d", dir, errno);
        ret = ESP_ERR_NOT_FOUND;
        goto out;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        bool is_dir = false;
        size_t size = 0;
        char full[300];
        int n = snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (n > 0 && (size_t)n < sizeof(full)) {
            struct stat st;
            if (stat(full, &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
                size = is_dir ? 0 : (size_t)st.st_size;
            }
        }
#ifdef DT_DIR
        if (!is_dir && ent->d_type == DT_DIR) { is_dir = true; size = 0; }
#endif
        cb(ent->d_name, size, is_dir, ctx);
    }
out:
    if (d) closedir(d);
    storage_usb_unlock();
    return ret;
}

#endif /* CONFIG_USB_MSC_HOST_ENABLE */
