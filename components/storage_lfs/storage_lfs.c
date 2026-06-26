/* LittleFS forrás-tár megvalósítás. (terv 11. szekció)
 *
 * Mount a "storage" partícióra /lfs alá, fájl-API (read_all/write_all/list)
 * és közös recursive mutex, mivel a webUI, az FTP és a prog_session is
 * egyszerre nyúlhat a fájlokhoz — a LittleFS nem feltétel nélkül thread-safe.
 */
#include "storage_lfs.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "storage_lfs";

/* A "storage" partíció címkéje a partitions.csv-ben. */
#define STORAGE_LFS_PARTITION "storage"

/* Közös recursive mutex — a lock egymásba ágyazható (pl. egy külső lock
   alatt hívott read_all is fogni próbálja), ezért RECURSIVE. */
static SemaphoreHandle_t s_mutex = NULL;

/* Egy alkönyvtár létrehozása; az EEXIST nem hiba. */
static esp_err_t ensure_dir(const char *path)
{
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir('%s') sikertelen: errno=%d", path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t storage_lfs_init(void)
{
    /* Mutex létrehozása (egyszer). */
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateRecursiveMutex();
        if (s_mutex == NULL) {
            ESP_LOGE(TAG, "recursive mutex létrehozása sikertelen");
            return ESP_ERR_NO_MEM;
        }
    }

    /* LittleFS mount a "storage" partícióra. */
    esp_vfs_littlefs_conf_t conf = {
        .base_path = STORAGE_LFS_BASE,
        .partition_label = STORAGE_LFS_PARTITION,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount sikertelen: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(STORAGE_LFS_PARTITION, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mountolva (%s): %u/%u bájt használt",
                 STORAGE_LFS_BASE, (unsigned)used, (unsigned)total);
    }

    /* Alapkönyvtárak létrehozása (EEXIST ignorálva). */
    esp_err_t derr = ESP_OK;
    if (ensure_dir(STORAGE_LFS_BASE "/fw")  != ESP_OK) derr = ESP_FAIL;
    if (ensure_dir(STORAGE_LFS_BASE "/cfg") != ESP_OK) derr = ESP_FAIL;
    if (ensure_dir(STORAGE_LFS_BASE "/www") != ESP_OK) derr = ESP_FAIL;
    if (derr != ESP_OK) {
        return derr;
    }

    return ESP_OK;
}

void storage_lfs_lock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    }
}

void storage_lfs_unlock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreGiveRecursive(s_mutex);
    }
}

esp_err_t storage_lfs_read_all(const char *path, void **buf, size_t *len)
{
    if (path == NULL || buf == NULL || len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *buf = NULL;
    *len = 0;

    esp_err_t ret = ESP_OK;
    void *data = NULL;
    FILE *f = NULL;

    storage_lfs_lock();

    f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen('%s', rb) sikertelen: errno=%d", path, errno);
        ret = ESP_ERR_NOT_FOUND;
        goto out;
    }

    /* Méret lekérése fseek/ftell-lel. */
    if (fseek(f, 0, SEEK_END) != 0) {
        ESP_LOGE(TAG, "fseek(END) sikertelen: '%s' errno=%d", path, errno);
        ret = ESP_FAIL;
        goto out;
    }
    long size = ftell(f);
    if (size < 0) {
        ESP_LOGE(TAG, "ftell sikertelen: '%s' errno=%d", path, errno);
        ret = ESP_FAIL;
        goto out;
    }
    rewind(f);

    /* Puffer foglalása — lehetőleg PSRAM-ba, fallback sima malloc.
       +1 bájt egy záró NUL-nak, hogy a hívó szöveges fájlt is biztonsággal
       kezelhessen; a visszaadott *len ezt nem tartalmazza. */
    size_t alloc = (size_t)size + 1;
    data = heap_caps_malloc(alloc, MALLOC_CAP_SPIRAM);
    if (data == NULL) {
        data = malloc(alloc);
    }
    if (data == NULL) {
        ESP_LOGE(TAG, "puffer foglalás sikertelen (%u bájt): '%s'",
                 (unsigned)alloc, path);
        ret = ESP_ERR_NO_MEM;
        goto out;
    }

    /* Beolvasás. */
    size_t rd = (size > 0) ? fread(data, 1, (size_t)size, f) : 0;
    if (rd != (size_t)size) {
        ESP_LOGE(TAG, "fread csonka: '%s' (%u/%ld)", path, (unsigned)rd, size);
        free(data);
        data = NULL;
        ret = ESP_FAIL;
        goto out;
    }

    ((uint8_t *)data)[size] = 0;  /* záró NUL a kényelemhez */

    *buf = data;
    *len = (size_t)size;
    data = NULL;  /* a hívó tulajdona, ne szabadítsuk fel */

out:
    if (f != NULL) {
        fclose(f);
    }
    storage_lfs_unlock();
    return ret;
}

esp_err_t storage_lfs_write_all(const char *path, const void *buf, size_t len)
{
    if (path == NULL || (buf == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    FILE *f = NULL;

    storage_lfs_lock();

    f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen('%s', wb) sikertelen: errno=%d", path, errno);
        ret = ESP_FAIL;
        goto out;
    }

    if (len > 0) {
        size_t wr = fwrite(buf, 1, len, f);
        if (wr != len) {
            ESP_LOGE(TAG, "fwrite csonka: '%s' (%u/%u)",
                     path, (unsigned)wr, (unsigned)len);
            ret = ESP_FAIL;
            goto out;
        }
    }

out:
    if (f != NULL) {
        if (fclose(f) != 0 && ret == ESP_OK) {
            ESP_LOGE(TAG, "fclose sikertelen: '%s' errno=%d", path, errno);
            ret = ESP_FAIL;
        }
    }
    storage_lfs_unlock();
    return ret;
}

esp_err_t storage_lfs_list(const char *dir, storage_lfs_list_cb cb, void *ctx)
{
    if (dir == NULL || cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    DIR *d = NULL;

    storage_lfs_lock();

    d = opendir(dir);
    if (d == NULL) {
        ESP_LOGE(TAG, "opendir('%s') sikertelen: errno=%d", dir, errno);
        ret = ESP_ERR_NOT_FOUND;
        goto out;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        /* A "." és ".." kihagyása. */
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        bool is_dir = false;
        size_t size = 0;

        /* Teljes elérési út a stat-hoz. */
        char full[256];
        int n = snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (n > 0 && (size_t)n < sizeof(full)) {
            struct stat st;
            if (stat(full, &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
                size = is_dir ? 0 : (size_t)st.st_size;
            }
        }

        /* Ha a stat nem adott típust, essünk vissza a dirent típusára. */
#ifdef DT_DIR
        if (!is_dir && ent->d_type == DT_DIR) {
            is_dir = true;
            size = 0;
        }
#endif

        cb(ent->d_name, size, is_dir, ctx);
    }

out:
    if (d != NULL) {
        closedir(d);
    }
    storage_lfs_unlock();
    return ret;
}
