/* AVR firmware-kép betöltő — közös réteg (Intel HEX / AVR ELF32-LE / nyers .bin).
 *
 * Eredetileg az avr_isp.c-ben volt; kiemelve, hogy az UPDI és PDI flow is
 * ugyanazt a parsert használja. Tisztán memóriában dolgozik, HW-független.
 */
#include "avr_image.h"

#include <string.h>
#include <ctype.h>
#include "esp_log.h"

static const char *TAG = "avr_image";

/* ============================ Intel HEX parser ========================== */

/* Kisbetűs kiterjesztés-egyezés (".hex"). */
static bool ends_with_hex(const char *path)
{
    size_t n = strlen(path);
    if (n < 4) return false;
    const char *e = path + n - 4;
    return (e[0] == '.' &&
            tolower((unsigned char)e[1]) == 'h' &&
            tolower((unsigned char)e[2]) == 'e' &&
            tolower((unsigned char)e[3]) == 'x');
}

static int hexnib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hexbyte(const char *s)
{
    int h = hexnib(s[0]), l = hexnib(s[1]);
    if (h < 0 || l < 0) return -1;
    return (h << 4) | l;
}

/* Intel HEX -> flash-kép. img: flash_size méretű puffer (0xFF-tel inicializálva).
 * *out_max: a legmagasabb beírt cím + 1 (a tényleges flash-kép hossza). */
static esp_err_t parse_intel_hex(const char *src, size_t src_len,
                                 uint8_t *img, size_t img_size, size_t *out_max)
{
    size_t max_addr = 0;
    uint32_t base = 0;           /* extended (segment/linear) cím-bázis */
    size_t i = 0;

    while (i < src_len) {
        /* Sor elejének megkeresése: ':' */
        while (i < src_len && src[i] != ':') i++;
        if (i >= src_len) break;
        i++;  /* ':' után */

        if (i + 8 > src_len) { ESP_LOGE(TAG, "HEX: csonka rekord-fej"); return ESP_FAIL; }
        int len = hexbyte(&src[i]);     i += 2;
        int ah  = hexbyte(&src[i]);     i += 2;
        int al  = hexbyte(&src[i]);     i += 2;
        int typ = hexbyte(&src[i]);     i += 2;
        if (len < 0 || ah < 0 || al < 0 || typ < 0) {
            ESP_LOGE(TAG, "HEX: ervenytelen hex-jegy a fejlecben");
            return ESP_FAIL;
        }
        uint16_t addr = (uint16_t)((ah << 8) | al);

        if (i + (size_t)(len + 1) * 2 > src_len) {
            ESP_LOGE(TAG, "HEX: csonka adat/checksum");
            return ESP_FAIL;
        }

        /* Checksum: a teljes rekord bájtösszege (LL+AA+AA+TT+adat+CC) == 0 mod 256. */
        uint8_t sum = (uint8_t)(len + ah + al + typ);
        uint8_t data[256];
        for (int b = 0; b < len; ++b) {
            int v = hexbyte(&src[i]); i += 2;
            if (v < 0) { ESP_LOGE(TAG, "HEX: ervenytelen adat-bajt"); return ESP_FAIL; }
            data[b] = (uint8_t)v;
            sum = (uint8_t)(sum + v);
        }
        int cc = hexbyte(&src[i]); i += 2;
        if (cc < 0) { ESP_LOGE(TAG, "HEX: ervenytelen checksum"); return ESP_FAIL; }
        if ((uint8_t)(sum + cc) != 0) {
            ESP_LOGE(TAG, "HEX: checksum hiba (rekord cim 0x%04X)", addr);
            return ESP_FAIL;
        }

        switch (typ) {
        case 0x00: {  /* adat */
            uint32_t full = base + addr;
            for (int b = 0; b < len; ++b) {
                uint32_t a = full + b;
                if (a >= img_size) {
                    ESP_LOGE(TAG, "HEX: cim 0x%lX tul nagy (flash %u B)",
                             (unsigned long)a, (unsigned)img_size);
                    return ESP_FAIL;
                }
                img[a] = data[b];
                if (a + 1 > max_addr) max_addr = a + 1;
            }
            break;
        }
        case 0x01:  /* EOF */
            goto done;
        case 0x02:  /* Extended Segment Address (paragrafus << 4) */
            if (len == 2) base = ((uint32_t)((data[0] << 8) | data[1])) << 4;
            break;
        case 0x04:  /* Extended Linear Address (felső 16 bit) */
            if (len == 2) base = ((uint32_t)((data[0] << 8) | data[1])) << 16;
            break;
        default:    /* 0x03/0x05 start address — AVR-nél figyelmen kívül */
            ESP_LOGV(TAG, "HEX: rekordtipus 0x%02X kihagyva", typ);
            break;
        }
    }
done:
    *out_max = max_addr;
    return ESP_OK;
}

/* ============================ AVR ELF32-LE parser ====================== */

/* Kisbetűs kiterjesztés-egyezés (".elf"). */
static bool ends_with_elf(const char *path)
{
    size_t n = strlen(path);
    if (n < 4) return false;
    const char *e = path + n - 4;
    return (e[0] == '.' &&
            tolower((unsigned char)e[1]) == 'e' &&
            tolower((unsigned char)e[2]) == 'l' &&
            tolower((unsigned char)e[3]) == 'f');
}

/* Little-endian olvasók a nyers ELF-bájtokból (struct.unpack '<H'/'<I'-szerűen). */
static uint16_t rd_u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

/* AVR-specifikus konstansok. */
#define ELF_PT_LOAD       1u
#define ELF_EM_AVR        83u        /* e_machine = 0x53 */
#define AVR_FLASH_VADDR_MAX 0x800000u /* a 0x800000+ az AVR RAM-alias, NEM flash */

/* AVR ELF32-LE -> flash-kép. img: flash_size méretű puffer (0xFF-tel inicializálva).
 * A betölthető (PT_LOAD) szegmensekből építjük fel a képet a p_paddr cím szerint,
 * a flash-tartományba (p_paddr < 0x800000) esőket másoljuk. *out_max: a legmagasabb
 * beírt cím + 1 (a tényleges flashelendő hossz). */
static esp_err_t parse_avr_elf(const uint8_t *src, size_t src_len,
                               uint8_t *img, size_t img_size, size_t *out_max)
{
    /* --- ELF32 fejléc-ellenőrzés (e_ident[16]) --- */
    if (src_len < 52) {  /* minimális ELF32 fejléc-méret */
        ESP_LOGE(TAG, "ELF: tul kicsi fajl (%u B)", (unsigned)src_len);
        return ESP_FAIL;
    }
    if (!(src[0] == 0x7F && src[1] == 'E' && src[2] == 'L' && src[3] == 'F')) {
        ESP_LOGE(TAG, "ELF: hibas magic (nem ELF)");
        return ESP_FAIL;
    }
    if (src[4] != 1 /* EI_CLASS = ELF32 */ || src[5] != 1 /* EI_DATA = LE */) {
        ESP_LOGE(TAG, "ELF: nem ELF32-LE (class=%u data=%u)", src[4], src[5]);
        return ESP_FAIL;
    }

    /* --- ELF32 fejléc-mezők (fix offszetek) --- */
    uint16_t e_machine   = rd_u16le(&src[0x12]);
    uint32_t e_phoff     = rd_u32le(&src[0x1C]);
    uint16_t e_phentsize = rd_u16le(&src[0x2A]);
    uint16_t e_phnum     = rd_u16le(&src[0x2C]);

    if (e_machine != ELF_EM_AVR) {
        /* Nem AVR gép — nem fatális, de figyelmeztetünk és folytatunk. */
        ESP_LOGW(TAG, "ELF: e_machine=%u (nem EM_AVR=83), folytatas", e_machine);
    } else {
        ESP_LOGD(TAG, "ELF: EM_AVR OK (e_machine=83)");
    }
    ESP_LOGD(TAG, "ELF: phoff=0x%lX phentsize=%u phnum=%u",
             (unsigned long)e_phoff, e_phentsize, e_phnum);

    if (e_phentsize < 32) {
        ESP_LOGE(TAG, "ELF: ervenytelen phentsize=%u", e_phentsize);
        return ESP_FAIL;
    }
    if (e_phnum == 0) {
        ESP_LOGE(TAG, "ELF: nincs program header (phnum=0)");
        return ESP_FAIL;
    }
    /* A program header tabla teljes egesze a fajlon belul legyen. */
    if (e_phoff > src_len ||
        (uint64_t)e_phoff + (uint64_t)e_phentsize * e_phnum > src_len) {
        ESP_LOGE(TAG, "ELF: program header tabla a fajlon kivul mutat");
        return ESP_FAIL;
    }

    /* --- Program headerek bejarasa, PT_LOAD szegmensek masolasa --- */
    size_t max_addr = 0;
    int loaded = 0;

    for (uint16_t ph = 0; ph < e_phnum; ++ph) {
        const uint8_t *p = src + e_phoff + (size_t)ph * e_phentsize;
        uint32_t p_type   = rd_u32le(&p[0]);
        uint32_t p_offset = rd_u32le(&p[4]);
        uint32_t p_vaddr  = rd_u32le(&p[8]);
        uint32_t p_paddr  = rd_u32le(&p[12]);
        uint32_t p_filesz = rd_u32le(&p[16]);
        uint32_t p_memsz  = rd_u32le(&p[20]);

        ESP_LOGV(TAG, "PH[%u]: type=%lu off=0x%lX vaddr=0x%lX paddr=0x%lX filesz=%lu memsz=%lu",
                 ph, (unsigned long)p_type, (unsigned long)p_offset,
                 (unsigned long)p_vaddr, (unsigned long)p_paddr,
                 (unsigned long)p_filesz, (unsigned long)p_memsz);

        if (p_type != ELF_PT_LOAD || p_filesz == 0) continue;

        /* Csak a flash-tartomany (paddr < 0x800000); a 0x800000+ a RAM-alias. */
        if (p_paddr >= AVR_FLASH_VADDR_MAX) {
            ESP_LOGD(TAG, "PT_LOAD paddr=0x%lX kihagyva (RAM-alias, nem flash)",
                     (unsigned long)p_paddr);
            continue;
        }

        /* Bounds-check: a forrasbol olvasott tartomany a fajlon belul. */
        if ((uint64_t)p_offset + p_filesz > src_len) {
            ESP_LOGE(TAG, "ELF: szegmens a fajlon kivul (off=0x%lX filesz=%lu fajl=%u)",
                     (unsigned long)p_offset, (unsigned long)p_filesz, (unsigned)src_len);
            return ESP_ERR_INVALID_SIZE;
        }
        /* Bounds-check: a cel-tartomany a flash-kep pufferen belul. */
        if ((uint64_t)p_paddr + p_filesz > img_size) {
            ESP_LOGE(TAG, "ELF: szegmens a flash-en kivul (paddr=0x%lX filesz=%lu flash=%u B)",
                     (unsigned long)p_paddr, (unsigned long)p_filesz, (unsigned)img_size);
            return ESP_ERR_INVALID_SIZE;
        }

        memcpy(img + p_paddr, src + p_offset, p_filesz);
        if ((size_t)(p_paddr + p_filesz) > max_addr) max_addr = p_paddr + p_filesz;
        loaded++;
        ESP_LOGI(TAG, "PT_LOAD paddr=0x%lx off=0x%lx filesz=%u -> flash",
                 (unsigned long)p_paddr, (unsigned long)p_offset, (unsigned)p_filesz);
        (void)p_vaddr; (void)p_memsz;
    }

    if (loaded == 0) {
        ESP_LOGW(TAG, "ELF: nincs flashelendo PT_LOAD szegmens");
    }

    *out_max = max_addr;
    return ESP_OK;
}

/* ============================ Diszpécser =============================== */
esp_err_t avr_image_parse(const char *path, const void *raw, size_t raw_len,
                          uint8_t *img, size_t img_size, size_t *out_len)
{
    if (!path || !raw || !img || !out_len) return ESP_ERR_INVALID_ARG;

    if (ends_with_hex(path)) {
        esp_err_t err = parse_intel_hex((const char *)raw, raw_len, img, img_size, out_len);
        if (err == ESP_OK) ESP_LOGI(TAG, "Intel HEX betoltve: %u B hasznos", (unsigned)*out_len);
        return err;
    }
    if (ends_with_elf(path)) {
        esp_err_t err = parse_avr_elf((const uint8_t *)raw, raw_len, img, img_size, out_len);
        if (err == ESP_OK) ESP_LOGI(TAG, "AVR ELF betoltve: %u B hasznos", (unsigned)*out_len);
        return err;
    }

    /* Nyers .bin 0-tól. */
    if (raw_len > img_size) {
        ESP_LOGE(TAG, "BIN tul nagy: %u B > flash %u B", (unsigned)raw_len, (unsigned)img_size);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(img, raw, raw_len);
    *out_len = raw_len;
    ESP_LOGI(TAG, "Nyers BIN betoltve: %u B", (unsigned)raw_len);
    return ESP_OK;
}
