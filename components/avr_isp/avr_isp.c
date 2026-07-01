/* AVR ISP programozó (ATtiny13 + rokon AVR-ek) — bit-bang SPI ISP.
 *
 * Teljesen független a SWD/FLM magtól. Az AVR-eket a klasszikus ISP
 * protokollon programozzuk: a célt RESET alatt tartva, SPI-szerű (mode 0,
 * MSB-first) 4-bájtos parancsokat küldünk bit-bankolva. A flash WORD-okból
 * (low+high bájt) áll, és FONTOS, hogy az ISP címek WORD-címek (nem bájt):
 * 1 word = 2 bájt.
 *
 * Lábak Kconfig-ból: SCK=GPIO15, MOSI=GPIO16, MISO=GPIO7, RESET=GPIO21.
 * Forrás LittleFS-ből: Intel HEX (.hex), AVR ELF32-LE (.elf) vagy nyers flash-kép (.bin).
 */
#include "avr_isp.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"   /* esp_rom_delay_us */
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "storage_lfs.h"
#include "storage_src.h"
#include "avr_image.h"   /* közös .hex/.elf/.bin képbetöltő */

static const char *TAG = "avr_isp";

/* ============================ Lábkiosztás (Kconfig) ======================== */
#define PIN_SCK    CONFIG_AVR_ISP_SCK_GPIO
#define PIN_MOSI   CONFIG_AVR_ISP_MOSI_GPIO
#define PIN_MISO   CONFIG_AVR_ISP_MISO_GPIO
#define PIN_RESET  CONFIG_AVR_ISP_RESET_GPIO

/* Fél-órajel késleltetés: ~2 us -> kb. 100-200 kHz ISP órajel. Az ATtiny13
 * belső 9.6/1.2 MHz oszcillátorához az ISP órajel < f_clk/4 legyen, ez bőven jó. */
#define ISP_HALF_US   3

/* ============================ Bit-bang SPI ================================ */

/* Egy bájt kiküldése MSB-first, közben a beérkező bájt összerakása (SPI mode 0:
 * MOSI beáll, felfutó él (SCK=1) -> minta, lefutó él (SCK=0)). */
static uint8_t isp_transfer(uint8_t out)
{
    uint8_t in = 0;
    for (int i = 7; i >= 0; --i) {
        gpio_set_level(PIN_MOSI, (out >> i) & 1);
        esp_rom_delay_us(ISP_HALF_US);
        gpio_set_level(PIN_SCK, 1);          /* felfutó él: cél mintavételezi a MOSI-t */
        esp_rom_delay_us(ISP_HALF_US);
        in = (uint8_t)((in << 1) | (gpio_get_level(PIN_MISO) & 1)); /* MISO minta a magas szakaszon */
        gpio_set_level(PIN_SCK, 0);          /* lefutó él */
    }
    return in;
}

/* Egy 4-bájtos ISP parancs. A visszaadott érték a 4. transfer eredménye
 * (a parancsok többségénél itt jön a hasznos válasz, pl. olvasásnál a bájt).
 * Az enter-programming visszhangjához a 3. transfer eredménye kell -> azt
 * külön kezeljük (lásd avr_enter_prog). */
static uint8_t isp_cmd(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    isp_transfer(a);
    isp_transfer(b);
    isp_transfer(c);
    uint8_t r = isp_transfer(d);
    ESP_LOGV(TAG, "ISP cmd %02X %02X %02X %02X -> %02X", a, b, c, d, r);
    return r;
}

/* ============================ Cél-tábla =================================== */
typedef struct {
    uint8_t  sig[3];
    const char *name;
    uint32_t flash_size;  /* bájt */
    uint16_t page_size;   /* bájt */
} avr_known_t;

static const avr_known_t AVR_TABLE[] = {
    { { 0x1E, 0x90, 0x07 }, "ATtiny13", 1024, 32 },
    { { 0x1E, 0x91, 0x08 }, "ATtiny25", 2048, 32 },
    { { 0x1E, 0x91, 0x09 }, "ATtiny26", 2048, 32 },
    { { 0x1E, 0x91, 0x0A }, "ATtiny2313", 2048, 32 },
    { { 0x1E, 0x91, 0x0B }, "ATtiny24", 2048, 32 },
    { { 0x1E, 0x91, 0x0C }, "ATtiny261", 2048, 32 },
    { { 0x1E, 0x92, 0x05 }, "ATmega48", 4096, 64 },
    { { 0x1E, 0x92, 0x06 }, "ATtiny45", 4096, 64 },
    { { 0x1E, 0x92, 0x07 }, "ATtiny44", 4096, 64 },
    { { 0x1E, 0x92, 0x08 }, "ATtiny461", 4096, 64 },
    { { 0x1E, 0x92, 0x09 }, "ATtiny48", 4096, 64 },
    { { 0x1E, 0x92, 0x0A }, "ATmega48P", 4096, 64 },
    { { 0x1E, 0x92, 0x0C }, "ATtiny43U", 4096, 64 },
    { { 0x1E, 0x92, 0x0D }, "ATtiny4313", 4096, 64 },
    { { 0x1E, 0x92, 0x10 }, "ATmega48PB", 4096, 64 },
    { { 0x1E, 0x92, 0x15 }, "ATtiny441", 4096, 16 },
    { { 0x1E, 0x93, 0x06 }, "ATmega8515", 8192, 64 },
    { { 0x1E, 0x93, 0x07 }, "ATmega8", 8192, 64 },
    { { 0x1E, 0x93, 0x08 }, "ATmega8535", 8192, 64 },
    { { 0x1E, 0x93, 0x0A }, "ATmega88", 8192, 64 },
    { { 0x1E, 0x93, 0x0B }, "ATtiny85", 8192, 64 },
    { { 0x1E, 0x93, 0x0C }, "ATtiny84", 8192, 64 },
    { { 0x1E, 0x93, 0x0D }, "ATtiny861", 8192, 64 },
    { { 0x1E, 0x93, 0x0F }, "ATmega88P", 8192, 64 },
    { { 0x1E, 0x93, 0x10 }, "ATmega8HVA", 8192, 128 },
    { { 0x1E, 0x93, 0x11 }, "ATtiny88", 8192, 64 },
    { { 0x1E, 0x93, 0x14 }, "ATtiny828", 8192, 64 },
    { { 0x1E, 0x93, 0x15 }, "ATtiny841", 8192, 16 },
    { { 0x1E, 0x93, 0x16 }, "ATmega88PB", 8192, 64 },
    { { 0x1E, 0x93, 0x81 }, "AT90PWM2", 8192, 64 },
    { { 0x1E, 0x93, 0x82 }, "AT90USB82", 8192, 128 },
    { { 0x1E, 0x93, 0x83 }, "AT90PWM1", 8192, 64 },
    { { 0x1E, 0x93, 0x87 }, "ATtiny87", 8192, 128 },
    { { 0x1E, 0x93, 0x88 }, "AT90PWM81", 8192, 64 },
    { { 0x1E, 0x93, 0x89 }, "ATmega8U2", 8192, 128 },
    { { 0x1E, 0x94, 0x01 }, "ATmega161", 16384, 128 },
    { { 0x1E, 0x94, 0x02 }, "ATmega163", 16384, 128 },
    { { 0x1E, 0x94, 0x03 }, "ATmega16", 16384, 128 },
    { { 0x1E, 0x94, 0x04 }, "ATmega162", 16384, 128 },
    { { 0x1E, 0x94, 0x05 }, "ATmega169", 16384, 128 },
    { { 0x1E, 0x94, 0x06 }, "ATmega168", 16384, 128 },
    { { 0x1E, 0x94, 0x07 }, "ATmega165", 16384, 128 },
    { { 0x1E, 0x94, 0x0A }, "ATmega164P", 16384, 128 },
    { { 0x1E, 0x94, 0x0B }, "ATmega168P", 16384, 128 },
    { { 0x1E, 0x94, 0x0C }, "ATmega16HVA", 16384, 128 },
    { { 0x1E, 0x94, 0x0D }, "ATmega16HVB", 16384, 128 },
    { { 0x1E, 0x94, 0x0F }, "ATmega164A", 16384, 128 },
    { { 0x1E, 0x94, 0x10 }, "ATmega165A", 16384, 128 },
    { { 0x1E, 0x94, 0x11 }, "ATmega169A", 16384, 128 },
    { { 0x1E, 0x94, 0x12 }, "ATtiny1634", 16384, 32 },
    { { 0x1E, 0x94, 0x15 }, "ATmega168PB", 16384, 128 },
    { { 0x1E, 0x94, 0x82 }, "AT90USB162", 16384, 128 },
    { { 0x1E, 0x94, 0x83 }, "AT90PWM216", 16384, 128 },
    { { 0x1E, 0x94, 0x84 }, "ATmega16M1", 16384, 128 },
    { { 0x1E, 0x94, 0x87 }, "ATtiny167", 16384, 128 },
    { { 0x1E, 0x94, 0x88 }, "ATmega16U4", 16384, 128 },
    { { 0x1E, 0x94, 0x89 }, "ATmega16U2", 16384, 128 },
    { { 0x1E, 0x94, 0x8B }, "AT90PWM161", 16384, 128 },
    { { 0x1E, 0x95, 0x02 }, "ATmega32", 32768, 128 },
    { { 0x1E, 0x95, 0x03 }, "ATmega329", 32768, 128 },
    { { 0x1E, 0x95, 0x04 }, "ATmega3290", 32768, 128 },
    { { 0x1E, 0x95, 0x05 }, "ATmega325", 32768, 128 },
    { { 0x1E, 0x95, 0x06 }, "ATmega3250", 32768, 128 },
    { { 0x1E, 0x95, 0x07 }, "ATmega406", 40960, 128 },
    { { 0x1E, 0x95, 0x08 }, "ATmega324P", 32768, 128 },
    { { 0x1E, 0x95, 0x0B }, "ATmega329P", 32768, 128 },
    { { 0x1E, 0x95, 0x0C }, "ATmega3290P", 32768, 128 },
    { { 0x1E, 0x95, 0x0D }, "ATmega325P", 32768, 128 },
    { { 0x1E, 0x95, 0x0E }, "ATmega3250P", 32768, 128 },
    { { 0x1E, 0x95, 0x0F }, "ATmega328P", 32768, 128 },
    { { 0x1E, 0x95, 0x10 }, "ATmega32HVB", 32768, 128 },
    { { 0x1E, 0x95, 0x11 }, "ATmega324PA", 32768, 128 },
    { { 0x1E, 0x95, 0x13 }, "ATmega32HVE2", 32768, 128 },
    { { 0x1E, 0x95, 0x14 }, "ATmega328", 32768, 128 },
    { { 0x1E, 0x95, 0x15 }, "ATmega324A", 32768, 128 },
    { { 0x1E, 0x95, 0x16 }, "ATmega328PB", 32768, 128 },
    { { 0x1E, 0x95, 0x17 }, "ATmega324PB", 32768, 128 },
    { { 0x1E, 0x95, 0x81 }, "AT90CAN32", 32768, 256 },
    { { 0x1E, 0x95, 0x84 }, "ATmega32M1", 32768, 128 },
    { { 0x1E, 0x95, 0x86 }, "ATmega32C1", 32768, 128 },
    { { 0x1E, 0x95, 0x87 }, "ATmega32U4", 32768, 128 },
    { { 0x1E, 0x95, 0x8A }, "ATmega32U2", 32768, 128 },
    { { 0x1E, 0x96, 0x02 }, "ATmega64", 65536, 256 },
    { { 0x1E, 0x96, 0x03 }, "ATmega649", 65536, 256 },
    { { 0x1E, 0x96, 0x04 }, "ATmega6490", 65536, 256 },
    { { 0x1E, 0x96, 0x05 }, "ATmega645", 65536, 256 },
    { { 0x1E, 0x96, 0x06 }, "ATmega6450", 65536, 256 },
    { { 0x1E, 0x96, 0x08 }, "ATmega640", 65536, 256 },
    { { 0x1E, 0x96, 0x09 }, "ATmega644", 65536, 256 },
    { { 0x1E, 0x96, 0x0A }, "ATmega644P", 65536, 256 },
    { { 0x1E, 0x96, 0x0B }, "ATmega649P", 65536, 256 },
    { { 0x1E, 0x96, 0x0C }, "ATmega6490P", 65536, 256 },
    { { 0x1E, 0x96, 0x0D }, "ATmega645P", 65536, 256 },
    { { 0x1E, 0x96, 0x0E }, "ATmega6450P", 65536, 256 },
    { { 0x1E, 0x96, 0x10 }, "ATmega64HVE2", 65536, 128 },
    { { 0x1E, 0x96, 0x81 }, "AT90CAN64", 65536, 256 },
    { { 0x1E, 0x96, 0x82 }, "AT90USB646", 65536, 256 },
    { { 0x1E, 0x96, 0x84 }, "ATmega64M1", 65536, 256 },
    { { 0x1E, 0x96, 0x86 }, "ATmega64C1", 65536, 256 },
    { { 0x1E, 0x97, 0x01 }, "ATmega103", 131072, 256 },
    { { 0x1E, 0x97, 0x02 }, "ATmega128", 131072, 256 },
    { { 0x1E, 0x97, 0x03 }, "ATmega1280", 131072, 256 },
    { { 0x1E, 0x97, 0x04 }, "ATmega1281", 131072, 256 },
    { { 0x1E, 0x97, 0x05 }, "ATmega1284P", 131072, 256 },
    { { 0x1E, 0x97, 0x06 }, "ATmega1284", 131072, 256 },
    { { 0x1E, 0x97, 0x81 }, "AT90CAN128", 131072, 256 },
    { { 0x1E, 0x97, 0x82 }, "AT90USB1286", 131072, 256 },
    { { 0x1E, 0x98, 0x01 }, "ATmega2560", 262144, 256 },
    { { 0x1E, 0x98, 0x02 }, "ATmega2561", 262144, 256 },
    { { 0x1E, 0xA6, 0x02 }, "ATmega64RFR2", 65536, 256 },
    { { 0x1E, 0xA6, 0x03 }, "ATmega644RFR2", 65536, 256 },
    { { 0x1E, 0xA7, 0x01 }, "ATmega128RFA1", 131072, 256 },
    { { 0x1E, 0xA7, 0x02 }, "ATmega128RFR2", 131072, 256 },
    { { 0x1E, 0xA7, 0x03 }, "ATmega1284RFR2", 131072, 256 },
    { { 0x1E, 0xA8, 0x02 }, "ATmega256RFR2", 262144, 256 },
    { { 0x1E, 0xA8, 0x03 }, "ATmega2564RFR2", 262144, 256 },
};

static const avr_known_t *avr_lookup(const uint8_t sig[3])
{
    for (size_t i = 0; i < sizeof(AVR_TABLE) / sizeof(AVR_TABLE[0]); ++i) {
        if (memcmp(AVR_TABLE[i].sig, sig, 3) == 0) return &AVR_TABLE[i];
    }
    return NULL;
}

/* ============================ GPIO init ================================== */
esp_err_t avr_isp_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_SCK) | (1ULL << PIN_MOSI) | (1ULL << PIN_RESET),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&out);
    if (err != ESP_OK) return err;

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_MISO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   /* lebegés ellen, nem kötelező */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&in);
    if (err != ESP_OK) return err;

    /* Idle állapot: SCK alacsony, RESET magas (cél fut), MOSI alacsony. */
    gpio_set_level(PIN_SCK, 0);
    gpio_set_level(PIN_MOSI, 0);
    gpio_set_level(PIN_RESET, 1);

    ESP_LOGI(TAG, "AVR ISP init: SCK=%d MOSI=%d MISO=%d RESET=%d",
             PIN_SCK, PIN_MOSI, PIN_MISO, PIN_RESET);
    return ESP_OK;
}

/* ============================ ISP mód ==================================== */

/* RDY/BSY poll: 0xF0 parancs bit0 = 1 amíg foglalt. Timeout után visszatér. */
static void avr_wait_ready(int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    do {
        if ((isp_cmd(0xF0, 0x00, 0x00, 0x00) & 1) == 0) return;
        esp_rom_delay_us(100);
    } while (esp_timer_get_time() < deadline);
    ESP_LOGW(TAG, "RDY/BSY poll timeout (%d ms)", timeout_ms);
}

/* Programming mode-ba lépés: max ~32 próbálkozás reset-pulzussal.
 * A 'Programming Enable' parancs 3. bájtjának KÜLDÉSEKOR a cél visszhangozza
 * a 2. bájtot (0x53). Ezt a 3. transfer eredményeként figyeljük. */
static esp_err_t avr_enter_prog(void)
{
    for (int attempt = 0; attempt < 32; ++attempt) {
        /* Pozitív RESET-pulzus a szinkronhoz, majd RESET aktív (alacsony). */
        gpio_set_level(PIN_SCK, 0);
        gpio_set_level(PIN_RESET, 1);
        esp_rom_delay_us(50);
        gpio_set_level(PIN_RESET, 0);   /* aktív: cél reset alatt */
        esp_rom_delay_us(30000);        /* >20 ms várakozás a datasheet szerint */

        /* Programming Enable: 0xAC 0x53 0x00 0x00, a 3. transfer visszhang. */
        isp_transfer(0xAC);
        isp_transfer(0x53);
        uint8_t echo = isp_transfer(0x00);
        isp_transfer(0x00);

        if (echo == 0x53) {
            ESP_LOGI(TAG, "ISP enter OK (%d. probalkozas)", attempt + 1);
            return ESP_OK;
        }
        ESP_LOGV(TAG, "ISP enter visszhang=%02X (vart 0x53), ujraprobalom", echo);
    }
    ESP_LOGE(TAG, "ISP enter sikertelen 32 probalkozas utan (nincs cel?)");
    return ESP_FAIL;
}

/* Kilépés: RESET felengedése -> cél fut. */
static void avr_leave_prog(void)
{
    gpio_set_level(PIN_SCK, 0);
    gpio_set_level(PIN_RESET, 1);
    ESP_LOGI(TAG, "ISP leave: cel elengedve (RESET=1)");
}

/* Signature 3 bájt: 0x30 0x00 addr 0x00 -> bájt. */
static void avr_read_signature(uint8_t out[3])
{
    for (int a = 0; a < 3; ++a) {
        out[a] = isp_cmd(0x30, 0x00, (uint8_t)a, 0x00);
    }
}

/* ============================ Detect ==================================== */
esp_err_t avr_isp_detect(avr_dev_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    esp_err_t err = avr_enter_prog();
    if (err != ESP_OK) return err;

    avr_read_signature(out->sig);
    ESP_LOGI(TAG, "Signature: %02X %02X %02X", out->sig[0], out->sig[1], out->sig[2]);

    const avr_known_t *k = avr_lookup(out->sig);
    if (k) {
        out->known = true;
        out->name = k->name;
        out->flash_size = k->flash_size;
        out->page_size = k->page_size;
        ESP_LOGI(TAG, "Eszkoz: %s (flash %u B, lap %u B)",
                 k->name, (unsigned)k->flash_size, (unsigned)k->page_size);
    } else {
        out->known = false;
        out->name = NULL;
        ESP_LOGW(TAG, "Ismeretlen signature, nincs a tablaban");
    }

    avr_leave_prog();
    return ESP_OK;
}

/* ============================ Flash low-level ============================ */

/* Page buffer betöltése egy WORD-re (low + high bájt). wordaddr = lapon belüli
 * szó-index; az ISP a cím alsó bájtját kéri (a lap a magasabb bitekkel adott). */
static void avr_load_page_word(uint8_t word_lo, uint8_t lo, uint8_t hi)
{
    isp_cmd(0x40, 0x00, word_lo, lo);  /* low byte */
    isp_cmd(0x48, 0x00, word_lo, hi);  /* high byte */
}

/* Lap kiírása flash-be. pageaddr = WORD-cím (a lap első szavának címe). */
static void avr_write_page(uint16_t page_word_addr)
{
    isp_cmd(0x4C, (uint8_t)(page_word_addr >> 8), (uint8_t)(page_word_addr & 0xFF), 0x00);
    avr_wait_ready(20);
}

/* Egy flash-szó visszaolvasása (verify). word_addr = WORD-cím. */
static uint16_t avr_read_word(uint16_t word_addr)
{
    uint8_t lo = isp_cmd(0x20, (uint8_t)(word_addr >> 8), (uint8_t)(word_addr & 0xFF), 0x00);
    uint8_t hi = isp_cmd(0x28, (uint8_t)(word_addr >> 8), (uint8_t)(word_addr & 0xFF), 0x00);
    return (uint16_t)((hi << 8) | lo);
}

/* Chip erase: 0xAC 0x80 0x00 0x00, majd várakozás. */
static void avr_chip_erase(void)
{
    ESP_LOGD(TAG, "Chip erase");
    isp_cmd(0xAC, 0x80, 0x00, 0x00);
    avr_wait_ready(20);
}

/* ============================ Flash file =============================== */
esp_err_t avr_isp_flash_file(const char *path, avr_progress_cb cb, void *ctx)
{
    if (!path) return ESP_ERR_INVALID_ARG;

    /* 1) Forrásfájl beolvasása az aktív forrásból (LFS vagy USB; a hívó NEM,
          mi free-zünk). A teljes képet egyben olvassuk a memóriába. */
    void *raw = NULL;
    size_t raw_len = 0;
    esp_err_t err = storage_src_read_all(path, &raw, &raw_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fajl olvasas hiba: %s (%s)", path, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Forras: %s (%u B)", path, (unsigned)raw_len);

    uint8_t *img = NULL;
    esp_err_t ret = ESP_FAIL;

    /* 2) ISP-be lépés + detektálás (signature). */
    err = avr_enter_prog();
    if (err != ESP_OK) { free(raw); return err; }

    avr_dev_t dev = {0};
    avr_read_signature(dev.sig);
    ESP_LOGI(TAG, "Signature: %02X %02X %02X", dev.sig[0], dev.sig[1], dev.sig[2]);
    const avr_known_t *k = avr_lookup(dev.sig);

    uint32_t flash_size;
    uint16_t page_size;
    if (k) {
        dev.known = true; dev.name = k->name;
        flash_size = k->flash_size; page_size = k->page_size;
        ESP_LOGI(TAG, "Eszkoz: %s (flash %u B, lap %u B)",
                 k->name, (unsigned)flash_size, (unsigned)page_size);
    } else {
        /* Ismeretlen: folytatható, de óvatos defaultokkal (ATtiny13-szerű). */
        ESP_LOGW(TAG, "Ismeretlen signature — folytatas ovatos defaultokkal");
        flash_size = 1024; page_size = 32;
    }

    /* 3) Flash-kép puffer (0xFF = törölt flash). */
    img = malloc(flash_size);
    if (!img) { ESP_LOGE(TAG, "img malloc hiba (%u B)", (unsigned)flash_size); avr_leave_prog(); free(raw); return ESP_ERR_NO_MEM; }
    memset(img, 0xFF, flash_size);

    size_t img_len = 0;
    err = avr_image_parse(path, raw, raw_len, img, flash_size, &img_len);
    if (err != ESP_OK) { ESP_LOGE(TAG, "Kep betoltes hiba (%s)", esp_err_to_name(err)); ret = err; goto out; }

    if (img_len == 0) { ESP_LOGW(TAG, "Ures flash-kep, nincs mit irni"); ret = ESP_OK; goto out; }

    /* Lapra kerekítés: az utolsó lapot egészben írjuk (a maradék 0xFF). */
    size_t prog_len = ((img_len + page_size - 1) / page_size) * page_size;
    if (prog_len > flash_size) prog_len = flash_size;
    size_t total_pages = prog_len / page_size;
    ESP_LOGD(TAG, "Programozando: %u B, %u lap (lapmeret %u B)",
             (unsigned)prog_len, (unsigned)total_pages, (unsigned)page_size);

    /* >128 KB flash: a WORD-cím túllépi a 16 bitet, ehhez Load Extended Address
       (0x4D) kell — ez még nincs bekötve. A detektálás/név működik, de a
       flashelést itt tiszta hibával állítjuk le (silent korrupció helyett). */
    if (prog_len > 0x20000) {
        ESP_LOGE(TAG, ">128KB flash (%u B) extended addressing nelkul nem programozhato",
                 (unsigned)prog_len);
        ret = ESP_ERR_NOT_SUPPORTED; goto out;
    }

    /* 4) Chip erase. */
    if (cb) cb("Torles", 0, ctx);
    avr_chip_erase();
    if (cb) cb("Torles", 100, ctx);

    /* 5) Lap-programozás (load page buffer + write page). */
    uint16_t words_per_page = page_size / 2;
    for (size_t p = 0; p < total_pages; ++p) {
        size_t byte_base = p * page_size;
        uint16_t page_word_addr = (uint16_t)(byte_base / 2);

        for (uint16_t w = 0; w < words_per_page; ++w) {
            size_t bi = byte_base + (size_t)w * 2;
            uint8_t lo = img[bi];
            uint8_t hi = img[bi + 1];
            avr_load_page_word((uint8_t)(page_word_addr + w), lo, hi);
        }
        avr_write_page(page_word_addr);

        int pct = (int)(((p + 1) * 100) / total_pages);
        if (cb) cb("Iras", pct, ctx);
        ESP_LOGV(TAG, "Lap %u/%u kiirva (word 0x%04X)",
                 (unsigned)(p + 1), (unsigned)total_pages, page_word_addr);
    }
    ESP_LOGI(TAG, "Iras kesz: %u B", (unsigned)prog_len);

    /* 6) Verify: visszaolvasás + memcmp. */
    size_t total_words = prog_len / 2;
    for (size_t w = 0; w < total_words; ++w) {
        uint16_t got = avr_read_word((uint16_t)w);
        uint8_t lo = img[w * 2];
        uint8_t hi = img[w * 2 + 1];
        uint16_t exp = (uint16_t)((hi << 8) | lo);
        if (got != exp) {
            ESP_LOGE(TAG, "Verify hiba word 0x%04X: kapott %04X, vart %04X",
                     (unsigned)w, got, exp);
            ret = ESP_FAIL; goto out;
        }
        if ((w & 0x3F) == 0 || w + 1 == total_words) {
            int pct = (int)(((w + 1) * 100) / total_words);
            if (cb) cb("Ellenor.", pct, ctx);
        }
    }
    ESP_LOGI(TAG, "Verify OK: %u B egyezik", (unsigned)prog_len);
    ret = ESP_OK;

out:
    avr_leave_prog();
    if (img) free(img);
    free(raw);
    if (ret == ESP_OK) ESP_LOGI(TAG, "Flashelés kész: %s", path);
    return ret;
}
