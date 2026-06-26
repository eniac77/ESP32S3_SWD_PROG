/* flm_blobs.c — beépített FLM algoritmusok táblája.
 *
 * EGYELŐRE ÜRES TÁBLA: amíg nincs vendored .FLM (lásd flm_packs/README.md),
 * a tábla 0 elemű, hogy a build zöld maradjon. A flm_blobs_init() csak naplóz,
 * a flm_blobs_table() *count=0-t és üres tömböt ad vissza.
 *
 * Ide kerülnek majd a GENERÁLT tömbök:
 *   - a tools/flm_extract.py a vendored .FLM-ekből egy .c-t (és opc. .h-t) ír,
 *     amely a flm_blobs.h `flm_algo_t` / `flm_sector_t` típusait használja, és
 *     egy `const flm_algo_t * const g_flm_algos[]` + `g_flm_algos_count` párt ad.
 *   - akkor ezt a fájlt úgy kötjük be, hogy a generált táblát továbbadjuk
 *     (extern a generált .h-ból), és a flm_blobs_table() azt visszaadja.
 *
 * Tervezett CMake bekötés (NE aktiváld, amíg nincs .FLM a flm_packs/-ben):
 * ---------------------------------------------------------------------------
 *   # A flm_packs/-ben lévő .FLM-ek begyűjtése (a GLOB minta: flm_packs / csillag .FLM):
 *   file(GLOB FLM_SOURCES "${CMAKE_SOURCE_DIR}/flm_packs/[*].FLM")
 *   set(FLM_GEN_C  "${CMAKE_CURRENT_BINARY_DIR}/flm_generated.c")
 *   set(FLM_GEN_H  "${CMAKE_CURRENT_BINARY_DIR}/flm_generated.h")
 *
 *   add_custom_command(
 *       OUTPUT  ${FLM_GEN_C} ${FLM_GEN_H}
 *       COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/tools/flm_extract.py
 *               ${FLM_SOURCES}
 *               -o ${FLM_GEN_C} --header ${FLM_GEN_H}
 *       DEPENDS ${FLM_SOURCES} ${CMAKE_SOURCE_DIR}/tools/flm_extract.py
 *       COMMENT "FLM -> C generálás (flm_extract.py)"
 *       VERBATIM)
 *
 *   idf_component_register(
 *       SRCS "flm_blobs.c" ${FLM_GEN_C}
 *       INCLUDE_DIRS "." ${CMAKE_CURRENT_BINARY_DIR})
 * ---------------------------------------------------------------------------
 * A flm_blobs_table() ekkor a generált g_flm_algos[]/g_flm_algos_count-ot adja.
 */

#include "flm_blobs.h"
#include "esp_log.h"

static const char *TAG = "flm_blobs";

/* Üres tábla — nincs még vendored .FLM. Ha lesz generált g_flm_algos[],
   ezt a két szimbólumot a generált .h extern deklarációi váltják ki. */
static const flm_algo_t *const s_empty_table[] = {
    /* ide jönnek majd a &<algo>_algo pointerek (generált) */
    NULL,
};

esp_err_t flm_blobs_init(void)
{
    size_t count = 0;
    (void)flm_blobs_table(&count);
    ESP_LOGI(TAG, "init: %u beépített FLM algoritmus", (unsigned)count);
    return ESP_OK;
}

const flm_algo_t *const *flm_blobs_table(size_t *count)
{
    /* Egyelőre 0 elem. (Az s_empty_table[] egyetlen NULL-eleme csak azért van,
       hogy C-ben ne legyen 0 méretű tömb; a count szándékosan 0.) */
    if (count) {
        *count = 0;
    }
    return s_empty_table;
}
