/* flm_blobs.c — beépített flash-loader algoritmusok táblája.
 *
 * A tényleges táblát a tools/flm_extract.py generálja a STM32CubeProgrammer
 * .stldr (ST loader-ABI) belső flash loadereiből a flm_generated.c/.h fájlokba
 * (`g_flm_algos[]` + `g_flm_algos_count`). Ez a fájl csak továbbadja azt.
 *
 * Újragenerálás (a generált .c-t a CMakeLists.txt fordítja a flm_blobs.c mellé):
 *   <python> tools/flm_extract.py 0x431.stldr 0x413.stldr ... \
 *            -o components/flm_blobs/flm_generated.c \
 *            --header components/flm_blobs/flm_generated.h
 * A nyers .stldr fájlok ST-proprietary tartalom — NEM kerülnek a repóba.
 */

#include "flm_blobs.h"
#include "flm_generated.h"
#include "esp_log.h"

static const char *TAG = "flm_blobs";

esp_err_t flm_blobs_init(void)
{
    size_t count = 0;
    (void)flm_blobs_table(&count);
    ESP_LOGI(TAG, "init: %u beépített flash-loader algoritmus", (unsigned)count);
    return ESP_OK;
}

const flm_algo_t *const *flm_blobs_table(size_t *count)
{
    if (count) {
        *count = g_flm_algos_count;
    }
    return g_flm_algos;
}
