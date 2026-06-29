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

/* DBGMCU IDCODE regiszter lehetséges címei. A cím a Cortex-magtól / családtól
 * függ; a hívó ezeket sorban végigpróbálja, amíg ismert DEV_ID-t kap. A
 * detektálás CSAK akkor fogad el egy találatot, ha a beolvasott cím egyezik a
 * tábla-bejegyzés dbgmcu_idcode_addr mezőjével — így nem téveszt egy másik
 * memóriarégióból véletlenül "értelmesnek" tűnő érték.
 *   0xE0042000 — M3/M4/M7 klasszikus: F1, F2, F3, F4, F7, L1, L4, G4, WB, WL
 *   0x40015800 — M0/M0+: F0, L0, G0, C0, U0
 *   0xE0044000 — M33 (debugger/AHB-AP alias): L5, U5, H5, WBA
 *   0x5C001000 — STM32H7 (M7) DBGMCU
 * (Lásd OpenOCD stm32*.c + ST RMxxxx DBGMCU fejezetek.) */
static const uint32_t s_idcode_addrs[] = {
    0xE0042000u,  /* M3/M4/M7 klasszikus */
    0x40015800u,  /* M0/M0+ */
    0xE0044000u,  /* M33 (L5/U5/H5/WBA) */
    0x5C001000u,  /* STM32H7 */
};

/* Flash-size regiszter (Flash size data register) címek családonként /
 * eszközönként. A megadott címen egy 16 bites érték olvasható ki, ami a gyári
 * flash méretét adja kB-ban. A címek OpenOCD (stm32*.c) + ST RMxxxx "Flash
 * size data register" fejezetekből, keresztellenőrizve. FONTOS: futásidőben a
 * flash-méret NEM kritikus a flasheléshez — pontos DEV_ID-egyezésnél a
 * loader StorageInfo-ja adja a geometriát; ez az érték csak kijelzés/log. */
#define FSZ_F0      0x1FFFF7CCu  /* F0:  RM0091/RM0360 */
#define FSZ_F1      0x1FFFF7E0u  /* F1:  RM0008 (LD-n nem mindig gyári-programozott) */
#define FSZ_F2      0x1FFF7A22u  /* F2:  RM0033 (= F4 cím) */
#define FSZ_F3      0x1FFFF7CCu  /* F3:  RM0316/RM0364 */
#define FSZ_F4      0x1FFF7A22u  /* F4:  RM0090/RM0383/RM0401 */
#define FSZ_F7      0x1FF0F442u  /* F7 74x/75x/76x/77x: RM0385/RM0410 */
#define FSZ_F7_72x  0x1FF07A22u  /* F7 72x/73x: RM0431 (eltér!) */
#define FSZ_H7      0x1FF1E880u  /* H7 743/72x/73x: RM0433/RM0399/RM0431-H7 */
#define FSZ_H7_AB   0x08FFF80Cu  /* H7 7Ax/7Bx, 7Rx/7Sx: RM0455 (eltér!) */
#define FSZ_L0      0x1FF8007Cu  /* L0:  RM0451/RM0377 (NEM 0x1FF8004C — az L1) */
#define FSZ_L1_C12  0x1FF8004Cu  /* L1 Cat.1/2 (0x416/0x429): RM0038 */
#define FSZ_L1_C3   0x1FF800CCu  /* L1 Cat.3+ (0x427/0x436/0x437): RM0038 */
#define FSZ_L4      0x1FFF75E0u  /* L4/L4+: RM0351/RM0432 */
#define FSZ_G0      0x1FFF75E0u  /* G0:  RM0444 */
#define FSZ_G4      0x1FFF75E0u  /* G4:  RM0440 */
#define FSZ_C0      0x1FFF75A0u  /* C0:  RM0490 (NEM 0x1FFF75E0!) */
#define FSZ_U0_031  0x1FFF3EA0u  /* U0 U031 (0x459): RM0503 */
#define FSZ_U0_073  0x1FFF6EA0u  /* U0 U073/U083 (0x489): RM0503 */
#define FSZ_L5      0x0BFA05E0u  /* L5:  RM0438 */
#define FSZ_U5      0x0BFA07A0u  /* U5:  RM0456 */
#define FSZ_H5      0x08FFF80Cu  /* H5:  RM0481 */
#define FSZ_WBA5    0x0BF907A0u  /* WBA5x (0x492): RM0493 */
#define FSZ_WBA6    0x0BFA07A0u  /* WBA6x (0x4B0): RM0493 (= U5 cím) */
#define FSZ_WB      0x1FFF75E0u  /* WB:  RM0434 */
#define FSZ_WL      0x1FFF75E0u  /* WL:  RM0453/RM0461 */

/* IDCODE-cím rövidítések (lásd s_idcode_addrs). */
#define IDC_M0  0x40015800u  /* F0/L0/G0/C0/U0 */
#define IDC_CL  0xE0042000u  /* F1/F2/F3/F4/F7/L1/L4/G4/WB/WL klasszikus */
#define IDC_M33 0xE0044000u  /* L5/U5/H5/WBA */
#define IDC_H7  0x5C001000u  /* H7 */

#define FLASH_BASE 0x08000000u

/* RDP / option-byte regiszterek családonkénti címei.
 * - F2/F4/F7: FLASH_OPTCR (RDP byte a [15:8] mezoben), @ 0x40023C14.
 * - L4/G0/G4/C0: FLASH_OPTR (RDP byte a [7:0] mezoben), @ 0x40022020.
 * - F0/F1/F3: FLASH_OBR (RDPRT mezo), @ 0x4002201C.
 * A tobbi (uj) csaladnal — H7/H5 (OPTSR, elteres + revizio-fuggo cim),
 * L0/L1 (forras-ellentmondas), L5/U5/U0/WBA (OPTR @ 0x40022040, M33-offset),
 * WB/WL (OPTR @ 0x58004020, eltero FLASH-bazis) — a cim ismert (lasd a tabla
 * megjegyzeseit), de szandekosan RDP_REG_NONE: HW-validacio nelkul egy rossz
 * cimrol olvasott szemet "level>=1"-kent BLOKKOLNA egy nyitott chip flashelest.
 * A flashelhetoseget ez nem erinti (RDP NONE -> a session folytat). */
#define RDP_OPTCR_F4 0x40023C14u  /* F2/F4/F7 FLASH_OPTCR @ 0x40023C14 */
#define RDP_OPTR_L4  0x40022020u  /* L4/G0/G4/C0 FLASH_OPTR @ 0x40022020 */
#define RDP_OBR_F1   0x4002201Cu  /* F0/F1/F3 FLASH_OBR @ 0x4002201C */

/* A cél-adatbázis statikus táblája. Egy sor = egy ismert DEV_ID variáns.
 * DEV_ID-sorrendben. A leképezés OpenOCD (stm32f1x/f2x/h7x/l4x/lx.c) + stlink
 * stm32.h + ST RMxxxx keresztellenőrzéssel készült. Mind a 80 sorhoz van
 * generált flash-loader (flm_generated.c), így a pontos DEV_ID-egyezés a
 * loadert megtalálja és a cél flashelhető. */
static const target_info_t s_targets[] = {
    /* ---- STM32F0 (M0), IDCODE @ 0x40015800, half-word, RDP=OBR ---- */
    { STM32_FAM_F0, "STM32F030x8/F05x",        0x440, IDC_M0, FSZ_F0, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F0, "STM32F09x/F030xC",        0x442, IDC_M0, FSZ_F0, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F0, "STM32F03x",               0x444, IDC_M0, FSZ_F0, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F0, "STM32F04x/F070x6",        0x445, IDC_M0, FSZ_F0, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F0, "STM32F070x8/F07x",        0x448, IDC_M0, FSZ_F0, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },

    /* ---- STM32F1 (M3), IDCODE @ 0xE0042000, half-word, RDP=OBR ---- */
    { STM32_FAM_F1, "STM32F101/2/3 MD",        0x410, IDC_CL, FSZ_F1, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F1, "STM32F101/2/3 LD",        0x412, IDC_CL, FSZ_F1, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F1, "STM32F101/3 HD",          0x414, IDC_CL, FSZ_F1, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F1, "STM32F105/107 (CL)",      0x418, IDC_CL, FSZ_F1, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F1, "STM32F100 LD/MD (VL)",    0x420, IDC_CL, FSZ_F1, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F1, "STM32F100 HD (VL)",       0x428, IDC_CL, FSZ_F1, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F1, "STM32F101/3 XL",          0x430, IDC_CL, FSZ_F1, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },

    /* ---- STM32F2 (M3), IDCODE @ 0xE0042000, word, RDP=OPTCR ---- */
    { STM32_FAM_F2, "STM32F2xx",               0x411, IDC_CL, FSZ_F2, FLASH_BASE, "word (PSIZE)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },

    /* ---- STM32F3 (M4), IDCODE @ 0xE0042000, half-word, RDP=OBR ---- */
    { STM32_FAM_F3, "STM32F302/303xB/C, F358", 0x422, IDC_CL, FSZ_F3, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F3, "STM32F373/378",           0x432, IDC_CL, FSZ_F3, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F3, "STM32F303x6/8, F328/334", 0x438, IDC_CL, FSZ_F3, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F3, "STM32F301/302x6/8, F318", 0x439, IDC_CL, FSZ_F3, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },
    { STM32_FAM_F3, "STM32F302/303xD/E, F398", 0x446, IDC_CL, FSZ_F3, FLASH_BASE, "half-word", RDP_OBR_F1, RDP_REG_OBR_F1 },

    /* ---- STM32F4 (M4), IDCODE @ 0xE0042000, word/sector, RDP=OPTCR ---- */
    { STM32_FAM_F4, "STM32F405/407/415/417",   0x413, IDC_CL, FSZ_F4, FLASH_BASE, "word (PSIZE)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },
    { STM32_FAM_F4, "STM32F42x/43x",           0x419, IDC_CL, FSZ_F4, FLASH_BASE, "word (PSIZE)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },
    { STM32_FAM_F4, "STM32F446",               0x421, IDC_CL, FSZ_F4, FLASH_BASE, "word (PSIZE)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },
    { STM32_FAM_F4, "STM32F401xB/C",           0x423, IDC_CL, FSZ_F4, FLASH_BASE, "word (PSIZE)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },
    { STM32_FAM_F4, "STM32F411",               0x431, IDC_CL, FSZ_F4, FLASH_BASE, "word (PSIZE)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },
    { STM32_FAM_F4, "STM32F401xD/E",           0x433, IDC_CL, FSZ_F4, FLASH_BASE, "word (PSIZE)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },
    { STM32_FAM_F4, "STM32F469/479",           0x434, IDC_CL, FSZ_F4, FLASH_BASE, "word (PSIZE)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },
    { STM32_FAM_F4, "STM32F412",               0x441, IDC_CL, FSZ_F4, FLASH_BASE, "word (PSIZE)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },
    { STM32_FAM_F4, "STM32F410",               0x458, IDC_CL, FSZ_F4, FLASH_BASE, "word (PSIZE)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },
    { STM32_FAM_F4, "STM32F413/423",           0x463, IDC_CL, FSZ_F4, FLASH_BASE, "word (PSIZE)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },

    /* ---- STM32F7 (M7), IDCODE @ 0xE0042000, word, RDP=OPTCR ---- */
    { STM32_FAM_F7, "STM32F74x/75x",           0x449, IDC_CL, FSZ_F7,     FLASH_BASE, "word (ITCM/AXIM)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },
    { STM32_FAM_F7, "STM32F76x/77x",           0x451, IDC_CL, FSZ_F7,     FLASH_BASE, "word (ITCM/AXIM)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },
    { STM32_FAM_F7, "STM32F72x/73x",           0x452, IDC_CL, FSZ_F7_72x, FLASH_BASE, "word (ITCM/AXIM)", RDP_OPTCR_F4, RDP_REG_OPTCR_F4 },

    /* ---- STM32H7 (M7), IDCODE @ 0x5C001000, word; RDP=OPTSR -> NONE ---- */
    { STM32_FAM_H7, "STM32H742/743/75x/74x",   0x450, IDC_H7, FSZ_H7,    FLASH_BASE, "word (256-bit flash)", 0, RDP_REG_NONE },
    { STM32_FAM_H7, "STM32H7Ax/7Bx",           0x480, IDC_H7, FSZ_H7_AB, FLASH_BASE, "word (256-bit flash)", 0, RDP_REG_NONE },
    { STM32_FAM_H7, "STM32H72x/73x",           0x483, IDC_H7, FSZ_H7,    FLASH_BASE, "word (256-bit flash)", 0, RDP_REG_NONE },
    { STM32_FAM_H7, "STM32H7Rx/7Sx",           0x485, IDC_H7, FSZ_H7_AB, FLASH_BASE, "word (256-bit flash)", 0, RDP_REG_NONE },

    /* ---- STM32H5 (M33), IDCODE @ 0xE0044000; RDP=OPTSR/PRODUCT_STATE -> NONE ---- */
    { STM32_FAM_H5, "STM32H503",               0x474, IDC_M33, FSZ_H5, FLASH_BASE, "quad-word", 0, RDP_REG_NONE },
    { STM32_FAM_H5, "STM32H523/533",           0x478, IDC_M33, FSZ_H5, FLASH_BASE, "quad-word", 0, RDP_REG_NONE },
    { STM32_FAM_H5, "STM32H562/563/573",       0x484, IDC_M33, FSZ_H5, FLASH_BASE, "quad-word", 0, RDP_REG_NONE },

    /* ---- STM32L0 (M0+), IDCODE @ 0x40015800, page/half-page; RDP -> NONE ---- */
    { STM32_FAM_L0, "STM32L0xx Cat.1 (L01/02)", 0x457, IDC_M0, FSZ_L0, FLASH_BASE, "page/half-page", 0, RDP_REG_NONE },
    { STM32_FAM_L0, "STM32L0xx Cat.2 (L03/04)", 0x425, IDC_M0, FSZ_L0, FLASH_BASE, "page/half-page", 0, RDP_REG_NONE },
    { STM32_FAM_L0, "STM32L0xx Cat.3 (L05/06)", 0x417, IDC_M0, FSZ_L0, FLASH_BASE, "page/half-page", 0, RDP_REG_NONE },
    { STM32_FAM_L0, "STM32L0xx Cat.5 (L07/08)", 0x447, IDC_M0, FSZ_L0, FLASH_BASE, "page/half-page", 0, RDP_REG_NONE },

    /* ---- STM32L1 (M3), IDCODE @ 0xE0042000, page; flash-size kat.-függő; RDP -> NONE ---- */
    { STM32_FAM_L1, "STM32L1xx Cat.1",         0x416, IDC_CL, FSZ_L1_C12, FLASH_BASE, "page/half-page", 0, RDP_REG_NONE },
    { STM32_FAM_L1, "STM32L1xx Cat.2",         0x429, IDC_CL, FSZ_L1_C12, FLASH_BASE, "page/half-page", 0, RDP_REG_NONE },
    { STM32_FAM_L1, "STM32L1xx Cat.3",         0x427, IDC_CL, FSZ_L1_C3,  FLASH_BASE, "page/half-page", 0, RDP_REG_NONE },
    { STM32_FAM_L1, "STM32L1xx Cat.4",         0x436, IDC_CL, FSZ_L1_C3,  FLASH_BASE, "page/half-page", 0, RDP_REG_NONE },
    { STM32_FAM_L1, "STM32L1xx Cat.5/6",       0x437, IDC_CL, FSZ_L1_C3,  FLASH_BASE, "page/half-page", 0, RDP_REG_NONE },

    /* ---- STM32L4 / L4+ (M4), IDCODE @ 0xE0042000, double-word, RDP=OPTR ---- */
    { STM32_FAM_L4, "STM32L47x/48x",           0x415, IDC_CL, FSZ_L4, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_L4, "STM32L43x/44x",           0x435, IDC_CL, FSZ_L4, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_L4, "STM32L49x/4Ax",           0x461, IDC_CL, FSZ_L4, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_L4, "STM32L45x/46x",           0x462, IDC_CL, FSZ_L4, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_L4, "STM32L41x/42x",           0x464, IDC_CL, FSZ_L4, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_L4, "STM32L4Rx/4Sx (L4+)",     0x470, IDC_CL, FSZ_L4, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_L4, "STM32L4Px/4Qx (L4+)",     0x471, IDC_CL, FSZ_L4, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },

    /* ---- STM32L5 (M33), IDCODE @ 0xE0044000; RDP=OPTR @0x40022040 -> NONE ---- */
    { STM32_FAM_L5, "STM32L552/562",           0x472, IDC_M33, FSZ_L5, FLASH_BASE, "double-word", 0, RDP_REG_NONE },

    /* ---- STM32U0 (M0+), IDCODE @ 0x40015800; flash-size eszközfüggő; RDP -> NONE ---- */
    { STM32_FAM_U0, "STM32U031",               0x459, IDC_M0, FSZ_U0_031, FLASH_BASE, "page", 0, RDP_REG_NONE },
    { STM32_FAM_U0, "STM32U073/083",           0x489, IDC_M0, FSZ_U0_073, FLASH_BASE, "page", 0, RDP_REG_NONE },

    /* ---- STM32U5 (M33), IDCODE @ 0xE0044000; RDP=OPTR @0x40022040 -> NONE ---- */
    { STM32_FAM_U5, "STM32U535/545",           0x454, IDC_M33, FSZ_U5, FLASH_BASE, "quad-word", 0, RDP_REG_NONE },
    { STM32_FAM_U5, "STM32U535/545",           0x455, IDC_M33, FSZ_U5, FLASH_BASE, "quad-word", 0, RDP_REG_NONE },
    { STM32_FAM_U5, "STM32U5F/5G",             0x476, IDC_M33, FSZ_U5, FLASH_BASE, "quad-word", 0, RDP_REG_NONE },
    { STM32_FAM_U5, "STM32U59x/5Ax",           0x481, IDC_M33, FSZ_U5, FLASH_BASE, "quad-word", 0, RDP_REG_NONE },
    { STM32_FAM_U5, "STM32U575/585",           0x482, IDC_M33, FSZ_U5, FLASH_BASE, "quad-word", 0, RDP_REG_NONE },

    /* ---- STM32G0 (M0+), IDCODE @ 0x40015800, double-word, RDP=OPTR ---- */
    { STM32_FAM_G0, "STM32G05x/06x",           0x456, IDC_M0, FSZ_G0, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_G0, "STM32G07x/08x",           0x460, IDC_M0, FSZ_G0, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_G0, "STM32G03x/04x",           0x466, IDC_M0, FSZ_G0, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_G0, "STM32G0Bx/0Cx",           0x467, IDC_M0, FSZ_G0, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },

    /* ---- STM32G4 (M4), IDCODE @ 0xE0042000, double-word, RDP=OPTR ---- */
    { STM32_FAM_G4, "STM32G43x/44x",           0x468, IDC_CL, FSZ_G4, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_G4, "STM32G47x/48x",           0x469, IDC_CL, FSZ_G4, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_G4, "STM32G49x/4Ax",           0x479, IDC_CL, FSZ_G4, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },

    /* ---- STM32C0 (M0+), IDCODE @ 0x40015800, double-word, RDP=OPTR ---- */
    { STM32_FAM_C0, "STM32C011",               0x443, IDC_M0, FSZ_C0, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_C0, "STM32C031",               0x453, IDC_M0, FSZ_C0, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_C0, "STM32C051",               0x44C, IDC_M0, FSZ_C0, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_C0, "STM32C071",               0x493, IDC_M0, FSZ_C0, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },
    { STM32_FAM_C0, "STM32C091/092",           0x44D, IDC_M0, FSZ_C0, FLASH_BASE, "double-word", RDP_OPTR_L4, RDP_REG_OPTR_L4 },

    /* ---- STM32WB (M4), IDCODE @ 0xE0042000; RDP=OPTR @0x58004020 -> NONE ---- */
    { STM32_FAM_WB, "STM32WB10/15",            0x494, IDC_CL, FSZ_WB, FLASH_BASE, "double-word", 0, RDP_REG_NONE },
    { STM32_FAM_WB, "STM32WB30/35/50/55",      0x495, IDC_CL, FSZ_WB, FLASH_BASE, "double-word", 0, RDP_REG_NONE },
    { STM32_FAM_WB, "STM32WB35",               0x496, IDC_CL, FSZ_WB, FLASH_BASE, "double-word", 0, RDP_REG_NONE },

    /* ---- STM32WL (M4), IDCODE @ 0xE0042000; RDP=OPTR @0x58004020 -> NONE ---- */
    { STM32_FAM_WL, "STM32WLE5/WL55",          0x497, IDC_CL, FSZ_WL, FLASH_BASE, "double-word", 0, RDP_REG_NONE },

    /* ---- STM32WBA (M33), IDCODE @ 0xE0044000; RDP=OPTR @0x40022040 -> NONE ---- */
    { STM32_FAM_WBA, "STM32WBA5x",             0x492, IDC_M33, FSZ_WBA5, FLASH_BASE, "double-word", 0, RDP_REG_NONE },
    { STM32_FAM_WBA, "STM32WBA6x",             0x4B0, IDC_M33, FSZ_WBA6, FLASH_BASE, "double-word", 0, RDP_REG_NONE },
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
        case STM32_FAM_F0:  return "F0";
        case STM32_FAM_F1:  return "F1";
        case STM32_FAM_F2:  return "F2";
        case STM32_FAM_F3:  return "F3";
        case STM32_FAM_F4:  return "F4";
        case STM32_FAM_F7:  return "F7";
        case STM32_FAM_H7:  return "H7";
        case STM32_FAM_H5:  return "H5";
        case STM32_FAM_G0:  return "G0";
        case STM32_FAM_G4:  return "G4";
        case STM32_FAM_L0:  return "L0";
        case STM32_FAM_L1:  return "L1";
        case STM32_FAM_L4:  return "L4";
        case STM32_FAM_L5:  return "L5";
        case STM32_FAM_U0:  return "U0";
        case STM32_FAM_U5:  return "U5";
        case STM32_FAM_WB:  return "WB";
        case STM32_FAM_WL:  return "WL";
        case STM32_FAM_WBA: return "WBA";
        case STM32_FAM_C0:  return "C0";
        default:            return NULL;
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
