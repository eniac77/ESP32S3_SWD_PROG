/* Cél-adatbázis (target_db) implementáció.
 *
 * DBGMCU IDCODE / DEV_ID (alsó 12 bit) -> STM32 család + flash-size regiszter
 * címe + a célhoz illő FLM algoritmus kiválasztása.
 *
 * STM32 F0/F1/F3/F4/F7/L0/L1/L4/G0. (terv 10. szekció)
 *
 * Forrás: ST referencia-kézikönyvek (RMxxxx) DBGMCU IDCODE és
 * "Flash size data register" fejezetei. A flash-size regiszter címek
 * (FSZ_*) családonként ellenőrzöttek (lásd a makróknál a konkrét RM-et).
 * Ahol egy DEV_ID-besorolás bizonytalan, azt "TODO ellenőrizni" jelöli.
 */
#include <string.h>
#include "target_db.h"
#include "esp_log.h"

static const char *TAG = "target_db";

/* IDCODE regiszter lehetséges címei. A Cortex-M0/M0+ magú családoknál
 * (F0, L0, G0) az IDCODE a perifériatérben (0x40015800), a többi családnál
 * a privát perifériabuszon (0xE0042000) található. A hívó mindkettőt
 * végigpróbálja, amíg értelmes DEV_ID-t kap. */
static const uint32_t s_idcode_addrs[] = {
    0xE0042000u,  /* M3/M4/M7: F1, F3, F4, F7, L1, L4 */
    0x40015800u,  /* M0/M0+:  F0, L0, G0 */
};

/* Flash-size regiszter (Flash size data register) címek családonként.
 * A megadott címen egy 16 bites érték olvasható ki, ami a gyári flash
 * méretét adja kB-ban. A címek az ST referencia-kézikönyvekből (RMxxxx)
 * a "Flash size data register" / "Memory size register" fejezetekből
 * származnak, és ellenőrzöttek. */
#define FSZ_F0  0x1FFFF7CCu  /* RM0091/RM0360 F-size @ 0x1FFFF7CC (16-bit, kB) */
#define FSZ_F1  0x1FFFF7E0u  /* RM0008 Flash size reg @ 0x1FFFF7E0 (16-bit, kB) */
#define FSZ_F3  0x1FFFF7CCu  /* RM0316/RM0364 F-size @ 0x1FFFF7CC (16-bit, kB) */
#define FSZ_F4  0x1FFF7A22u  /* RM0090/RM0383/RM0401 F-size @ 0x1FFF7A22 (16-bit, kB) */
#define FSZ_F7  0x1FF0F442u  /* RM0385/RM0410 F-size @ 0x1FF0F442 (16-bit, kB) */
#define FSZ_L0  0x1FF8007Cu  /* RM0451/RM0377 Flash size reg @ 0x1FF8007C (16-bit, kB) */
#define FSZ_L1  0x1FF8004Cu  /* RM0038 F-size @ 0x1FF8004C (16-bit, kB); a korábbi 0x1FF800CC hibás volt */
#define FSZ_L4  0x1FFF75E0u  /* RM0351/RM0394 F-size @ 0x1FFF75E0 (16-bit, kB) */
#define FSZ_G0  0x1FFF75E0u  /* RM0444 F-size @ 0x1FFF75E0 (16-bit, kB) */

#define FLASH_BASE 0x08000000u

/* RDP / option-byte regiszterek családonkénti címei.
 * - F4/F7: FLASH_OPTCR (RDP byte a [15:8] mezoben).
 * - L4/G0: FLASH_OPTR  (RDP byte a [7:0] mezoben).
 * - F0/F1/F3: FLASH_OBR (RDPRT mezo).
 * (Lasd a megfelelo RMxxxx "Flash option control/status register" fejezeteket.) */
#define RDP_OPTCR_F4 0x40023C14u  /* F4/F7 FLASH_OPTCR @ 0x40023C14 */
#define RDP_OPTR_L4  0x40022020u  /* L4/G0 FLASH_OPTR  @ 0x40022020 */
#define RDP_OBR_F1   0x4002201Cu  /* F0/F1/F3 FLASH_OBR @ 0x4002201C */

/* A cél-adatbázis statikus táblája. Egy sor = egy ismert DEV_ID variáns. */
static const target_info_t s_targets[] = {
    /* ---- STM32F0 (Cortex-M0), IDCODE @ 0x40015800, half-word prog ---- */
    /* RDP: FLASH_OBR @ 0x4002201C (RDPRT mezo) */
    { STM32_FAM_F0, "STM32F030x8/F05x", 0x440, 0x40015800u, FSZ_F0, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F0, "STM32F03x",        0x444, 0x40015800u, FSZ_F0, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F0, "STM32F04x",        0x445, 0x40015800u, FSZ_F0, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F0, "STM32F07x",        0x448, 0x40015800u, FSZ_F0, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F0, "STM32F09x",        0x442, 0x40015800u, FSZ_F0, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },

    /* ---- STM32F1 (Cortex-M3), IDCODE @ 0xE0042000, half-word prog ---- */
    /* RDP: FLASH_OBR @ 0x4002201C (RDPRT mezo) */
    { STM32_FAM_F1, "STM32F103 medium", 0x410, 0xE0042000u, FSZ_F1, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F1, "STM32F1xx low",    0x412, 0xE0042000u, FSZ_F1, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F1, "STM32F1xx high",   0x414, 0xE0042000u, FSZ_F1, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F1, "STM32F1xx XL",     0x430, 0xE0042000u, FSZ_F1, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },

    /* ---- STM32F3 (Cortex-M4), IDCODE @ 0xE0042000, half-word prog ---- */
    /* RDP: FLASH_OBR @ 0x4002201C (RDPRT mezo) */
    { STM32_FAM_F3, "STM32F303xB/C",    0x422, 0xE0042000u, FSZ_F3, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F3, "STM32F303x6/8",    0x438, 0xE0042000u, FSZ_F3, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F3, "STM32F303xD/E",    0x446, 0xE0042000u, FSZ_F3, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F3, "STM32F37x",        0x432, 0xE0042000u, FSZ_F3, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },

    /* ---- STM32F4 (Cortex-M4), IDCODE @ 0xE0042000, sector erase ---- */
    /* RDP: FLASH_OPTCR @ 0x40023C14 (RDP byte [15:8]) */
    { STM32_FAM_F4, "STM32F405/407/415/417", 0x413, 0xE0042000u, FSZ_F4, FLASH_BASE, "word (PSIZE)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },
    { STM32_FAM_F4, "STM32F42x/43x",         0x419, 0xE0042000u, FSZ_F4, FLASH_BASE, "word (PSIZE)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },
    { STM32_FAM_F4, "STM32F401xB/C",         0x423, 0xE0042000u, FSZ_F4, FLASH_BASE, "word (PSIZE)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },
    { STM32_FAM_F4, "STM32F411",             0x431, 0xE0042000u, FSZ_F4, FLASH_BASE, "word (PSIZE)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },
    { STM32_FAM_F4, "STM32F401xD/E",         0x433, 0xE0042000u, FSZ_F4, FLASH_BASE, "word (PSIZE)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },
    { STM32_FAM_F4, "STM32F412",             0x441, 0xE0042000u, FSZ_F4, FLASH_BASE, "word (PSIZE)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },
    { STM32_FAM_F4, "STM32F446",             0x421, 0xE0042000u, FSZ_F4, FLASH_BASE, "word (PSIZE)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },
    { STM32_FAM_F4, "STM32F410",             0x458, 0xE0042000u, FSZ_F4, FLASH_BASE, "word (PSIZE)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },

    /* ---- STM32F7 (Cortex-M7), IDCODE @ 0xE0042000 ---- */
    /* RDP: FLASH_OPTCR @ 0x40023C14 (RDP byte [15:8]) */
    { STM32_FAM_F7, "STM32F74x/75x", 0x449, 0xE0042000u, FSZ_F7, FLASH_BASE, "word (ITCM/AXIM)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },
    { STM32_FAM_F7, "STM32F76x/77x", 0x451, 0xE0042000u, FSZ_F7, FLASH_BASE, "word (ITCM/AXIM)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },
    { STM32_FAM_F7, "STM32F72x/73x", 0x452, 0xE0042000u, FSZ_F7, FLASH_BASE, "word (ITCM/AXIM)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },

    /* ---- STM32L0 (Cortex-M0+), IDCODE @ 0x40015800, half-page write ---- */
    /* TODO: RDP cim ellenorizni (FLASH_OPTR cim/kodolas L0-n bizonytalan) -> NONE */
    { STM32_FAM_L0, "STM32L01x/02x", 0x457, 0x40015800u, FSZ_L0, FLASH_BASE, "page/half-page", 0, RDP_REG_NONE },
    { STM32_FAM_L0, "STM32L03x/04x", 0x425, 0x40015800u, FSZ_L0, FLASH_BASE, "page/half-page", 0, RDP_REG_NONE },
    { STM32_FAM_L0, "STM32L05x/06x", 0x417, 0x40015800u, FSZ_L0, FLASH_BASE, "page/half-page", 0, RDP_REG_NONE },
    { STM32_FAM_L0, "STM32L07x/08x", 0x447, 0x40015800u, FSZ_L0, FLASH_BASE, "page/half-page", 0, RDP_REG_NONE },

    /* ---- STM32L1 (Cortex-M3), IDCODE @ 0xE0042000, page flash ---- */
    /* TODO: RDP cim ellenorizni (FLASH_OBR/OPTR cim/kodolas L1-en bizonytalan) -> NONE */
    { STM32_FAM_L1, "STM32L1xx MD",   0x416, 0xE0042000u, FSZ_L1, FLASH_BASE, "page/half-page", 0, RDP_REG_NONE },
    { STM32_FAM_L1, "STM32L1xx MD+",  0x429, 0xE0042000u, FSZ_L1, FLASH_BASE, "page/half-page", 0, RDP_REG_NONE },
    { STM32_FAM_L1, "STM32L1xx HD",   0x427, 0xE0042000u, FSZ_L1, FLASH_BASE, "page/half-page", 0, RDP_REG_NONE },
    { STM32_FAM_L1, "STM32L1xx HD+",  0x436, 0xE0042000u, FSZ_L1, FLASH_BASE, "page/half-page", 0, RDP_REG_NONE },
    { STM32_FAM_L1, "STM32L1xx XL",   0x437, 0xE0042000u, FSZ_L1, FLASH_BASE, "page/half-page", 0, RDP_REG_NONE },

    /* ---- STM32L4 (Cortex-M4), IDCODE @ 0xE0042000, double-word ---- */
    /* RDP: FLASH_OPTR @ 0x40022020 (RDP byte [7:0]) */
    { STM32_FAM_L4, "STM32L47x/48x",  0x415, 0xE0042000u, FSZ_L4, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_L4, "STM32L43x/44x",  0x435, 0xE0042000u, FSZ_L4, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_L4, "STM32L45x/46x",  0x462, 0xE0042000u, FSZ_L4, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_L4, "STM32L41x/42x",  0x464, 0xE0042000u, FSZ_L4, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_L4, "STM32L4Rx/4Sx",  0x470, 0xE0042000u, FSZ_L4, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },

    /* ---- STM32G0 (Cortex-M0+), IDCODE @ 0x40015800, double-word ---- */
    /* RDP: FLASH_OPTR @ 0x40022020 (RDP byte [7:0]) */
    { STM32_FAM_G0, "STM32G07x/08x",  0x460, 0x40015800u, FSZ_G0, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_G0, "STM32G03x/04x",  0x466, 0x40015800u, FSZ_G0, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_G0, "STM32G0Bx/0Cx",  0x456, 0x40015800u, FSZ_G0, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_G0, "STM32G0B1",      0x467, 0x40015800u, FSZ_G0, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 }, /* TODO ellenőrizni (0x467 jellemzően a G0Bx/0Cx kat. azonosítója) */
};

#define TARGET_COUNT (sizeof(s_targets) / sizeof(s_targets[0]))

esp_err_t target_db_init(void)
{
    ESP_LOGI(TAG, "Cel-adatbazis inicializalva: %u ismert DEV_ID varians",
             (unsigned)TARGET_COUNT);
    return ESP_OK;
}

const target_info_t *target_db_lookup(uint16_t dev_id)
{
    /* Lineáris keresés a 12 bites DEV_ID-re; első találat. */
    uint16_t id = (uint16_t)(dev_id & 0x0FFFu);
    for (size_t i = 0; i < TARGET_COUNT; i++) {
        if (s_targets[i].dev_id == id) {
            return &s_targets[i];
        }
    }
    return NULL;
}

const uint32_t *target_db_idcode_addrs(size_t *n)
{
    if (n) {
        *n = sizeof(s_idcode_addrs) / sizeof(s_idcode_addrs[0]);
    }
    return s_idcode_addrs;
}

/* A család rövid kulcsa (pl. "F4"), amit a FLM algoritmus nevében keresünk
 * substringként a kiválasztáshoz. */
static const char *family_key(stm32_family_t fam)
{
    switch (fam) {
        case STM32_FAM_F0: return "F0";
        case STM32_FAM_F1: return "F1";
        case STM32_FAM_F3: return "F3";
        case STM32_FAM_F4: return "F4";
        case STM32_FAM_F7: return "F7";
        case STM32_FAM_L0: return "L0";
        case STM32_FAM_L1: return "L1";
        case STM32_FAM_L4: return "L4";
        case STM32_FAM_G0: return "G0";
        default:           return NULL;
    }
}

const flm_algo_t *target_db_select_flm(const target_info_t *info, uint32_t flash_size)
{
    if (!info) {
        return NULL;
    }

    size_t count = 0;
    const flm_algo_t * const *table = flm_blobs_table(&count);
    if (!table || count == 0) {
        /* Üres tábla: jelenleg ez a normális állapot (nincs vendored .FLM). */
        ESP_LOGD(TAG, "Ures FLM tabla -> nincs valaszthato algoritmus");
        return NULL;
    }

    /* Elsődleges: pontos DEV_ID egyezés. Az ST .stldr loaderek DEV_ID szerint
     * nevezettek és a generált tábla beállítja a dev_id-t — ez a legpontosabb. */
    if (info->dev_id != 0) {
        for (size_t i = 0; i < count; i++) {
            const flm_algo_t *a = table[i];
            if (a && a->dev_id == info->dev_id) {
                ESP_LOGI(TAG, "FLM valasztva DEV_ID alapjan: '%s' (0x%03X)",
                         a->name ? a->name : "?", (unsigned)a->dev_id);
                return a;
            }
        }
    }

    const char *key = family_key(info->family);
    if (!key) {
        return NULL;
    }

    /* Másodlagos: név-substring egyezés + méret-illeszkedés. A legszorosabb
     * (legkisebb elegendő dev_size) találatot választjuk, hogy a megfelelő
     * méret-variáns kerüljön elő, ne mindig a legnagyobb. */
    const flm_algo_t *best = NULL;
    for (size_t i = 0; i < count; i++) {
        const flm_algo_t *a = table[i];
        if (!a || !a->name) {
            continue;
        }
        /* Család-egyezés: a FLM neve tartalmazza a család kulcsát (pl. "F4"). */
        if (strstr(a->name, key) == NULL) {
            continue;
        }
        /* A tényleges flash méretnek bele kell férnie az algo lefedettségébe. */
        if (flash_size > a->dev_size) {
            continue;
        }
        if (best == NULL || a->dev_size < best->dev_size) {
            best = a;
        }
    }

    if (best) {
        ESP_LOGI(TAG, "FLM valasztva: '%s' (dev_size=%u) cel='%s' flash=%u",
                 best->name, (unsigned)best->dev_size, info->name, (unsigned)flash_size);
    } else {
        ESP_LOGW(TAG, "Nincs illeszkedo FLM: cel='%s' (kulcs=%s) flash=%u",
                 info->name, key, (unsigned)flash_size);
    }
    return best;
}

int target_db_rdp_level(const target_info_t *info, uint32_t reg_value)
{
    if (!info) {
        return -1;
    }

    switch (info->rdp_kind) {
        case RDP_REG_OPTCR_F4: {
            /* F4/F7 FLASH_OPTCR: RDP byte a [15:8] mezoben.
               0xAA -> level0 (vedtelen), 0xCC -> level2 (vegleges),
               minden mas ertek -> level1. */
            uint32_t rdp = (reg_value >> 8) & 0xFFu;
            if (rdp == 0xAA) return 0;
            if (rdp == 0xCC) return 2;
            return 1;
        }
        case RDP_REG_OPTR_L4: {
            /* L4/G0 FLASH_OPTR: RDP byte a [7:0] mezoben.
               0xAA -> level0, 0xCC -> level2, egyebkent level1. */
            uint32_t rdp = reg_value & 0xFFu;
            if (rdp == 0xAA) return 0;
            if (rdp == 0xCC) return 2;
            return 1;
        }
        case RDP_REG_OBR_F1: {
            /* F0/F1/F3 FLASH_OBR: az RDPRT (read protection) bit jelzi a
               vedettseget. Korlat: az OBR-bol a level2 nem mindig olvashato ki
               megbizhatoan, ezert itt csak level0 / level1 kulonboztetheto meg.
               RDPRT = bit1 (0x2). Ha be van allitva -> legalabb level1. */
            if (reg_value & 0x2u) return 1;
            return 0;
        }
        case RDP_REG_NONE:
        default:
            return -1;
    }
}
