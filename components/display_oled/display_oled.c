/*
 * display_oled.c — SSD1306 / SH1106 128x64 I2C OLED driver (ESP-IDF v5.5).
 *
 * Az NX80TESTER (AVR) ssd1306.c + display.c + mikrofont.c portja:
 *   - a framebuffer és a rajz-primitívek logikája VÁLTOZATLAN,
 *   - a font-renderelés logikája VÁLTOZATLAN (a pgm_read_byte helyett közvetlen
 *     tömb-indexelés, lásd oled_fonts.c),
 *   - az AVR TWI (twi_start/write/stop) I/O-t az ÚJ driver/i2c_master API
 *     váltja ki (i2c_master_bus_handle_t + i2c_master_dev_handle_t).
 *
 * A flush laponként (0..7) EGY I2C tranzakcióban küldi ki a 128 adatbájtot
 * (control 0x40 + adat), nem bájtonként — sokkal gyorsabb, mint az AVR.
 */
#include "display_oled.h"
#include "oled_fonts.h"

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>

/* ===================================================================== */
/* Konfiguráció — panel és I2C                                            */
/* ===================================================================== */

/* I2C lábak és busz (terv: SDA=GPIO8, SCL=GPIO9, I2C0, 400 kHz). */
#define OLED_I2C_PORT      I2C_NUM_0
#define OLED_I2C_SDA_GPIO  8
#define OLED_I2C_SCL_GPIO  9
#define OLED_I2C_HZ        400000

/* 7-bites I2C cím. Az AVR 0x78 ennek a 8-bites írási formája (0x3C<<1).
   Ha a modul 0x3D, írd át erre. */
#define OLED_I2C_ADDR      0x3C

/* Oszlop-eltolás: valódi SSD1306 = 0, SH1106 (1.3" panelek) = 2.
   A referencia panel SH1106 -> 2. (NX80 ssd1306.c: SSD_COL_OFFSET) */
#define OLED_COL_OFFSET    2

/* 180°-os forgatás (a referencia panel fejjel lefelé van szerelve):
     normál állás:   seg remap 0xA1, COM scan 0xC8
     180° fordított: seg remap 0xA0, COM scan 0xC0  <-- referencia
   A két byte-ot a fizikai szerelési orientációhoz kell igazítani. */
#define OLED_FLIP_180      1

#if OLED_FLIP_180
  #define OLED_SEG_REMAP   0xA0   /* segment remap (col0 -> SEG0) */
  #define OLED_COM_SCAN    0xC0   /* COM scan increment */
#else
  #define OLED_SEG_REMAP   0xA1   /* segment remap fordított */
  #define OLED_COM_SCAN    0xC8   /* COM scan decrement */
#endif

/* I2C control byte-ok */
#define OLED_CTRL_CMD      0x00   /* Co=0, D/C#=0 -> parancs(ok) */
#define OLED_CTRL_DATA     0x40   /* Co=0, D/C#=1 -> GDDRAM adat */

static const char *TAG = "display_oled";

/* ===================================================================== */
/* Belső állapot                                                         */
/* ===================================================================== */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

/* 1 KB statikus framebuffer (128 oszlop * 8 lap). (NX80 ssd1306.c: fb[]) */
static uint8_t s_fb[OLED_W * OLED_H / 8];

/* ===================================================================== */
/* Alacsonyszintű I2C kimenet (AVR twi_* helyett i2c_master)             */
/* ===================================================================== */

/* Egyetlen parancs-byte: control 0x00 + parancs. (NX80 cmd()) */
static esp_err_t oled_cmd(uint8_t c)
{
    uint8_t buf[2] = { OLED_CTRL_CMD, c };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), -1);
}

/* ===================================================================== */
/* Panel init / flush                                                    */
/* ===================================================================== */

static esp_err_t oled_panel_init(void)
{
    /* Init szekvencia az NX80 ssd1306.c-ből, a 2.3 szerinti forgatás-byte-okkal. */
    static const uint8_t seq[] = {
        0xAE,                  /* display off */
        0xD5, 0x80,            /* clock div */
        0xA8, 0x3F,            /* multiplex 1/64 -> 64 sor */
        0xD3, 0x00,            /* display offset */
        0x40,                  /* start line 0 */
        0x8D, 0x14,            /* charge pump on */
        0x20, 0x02,            /* memory mode: page (lap-cimzes) */
        OLED_SEG_REMAP,        /* segment remap (forgatás) */
        OLED_COM_SCAN,         /* COM scan irány (forgatás) */
        0xDA, 0x12,            /* COM pins */
        0x81, 0x7F,            /* contrast 127 (közepes) */
        0xD9, 0xF1,            /* precharge */
        0xDB, 0x40,            /* VCOM detect */
        0xA4,                  /* resume to RAM content */
        0xA6,                  /* normal (nem invertalt) */
        0xAF                   /* display on */
    };

    /* Várakozás a panel tápfeszültségére (AVR busy-loop helyett vTaskDelay). */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* A teljes init EGY tranzakcióban: control 0x00 + a teljes seq[]. */
    uint8_t buf[1 + sizeof(seq)];
    buf[0] = OLED_CTRL_CMD;
    memcpy(&buf[1], seq, sizeof(seq));
    return i2c_master_transmit(s_dev, buf, sizeof(buf), -1);
}

void display_oled_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void display_oled_flush(void)
{
    /* Lap-cimzes: minden laphoz beallitjuk a lapot + oszlop-kezdocimet,
       majd EGY tranzakcioban kiirjuk a 128 adatbajtot. Robusztus, fuggetlen
       a cimzesi modtol (SSD1306 / SH1106 egyarant). */
    for (uint8_t page = 0; page < 8; page++) {
        uint8_t hdr[4] = {
            OLED_CTRL_CMD,
            (uint8_t)(0xB0 | page),                       /* lap kivalasztasa */
            (uint8_t)(0x00 | (OLED_COL_OFFSET & 0x0F)),   /* oszlop also nibble */
            (uint8_t)(0x10 | (OLED_COL_OFFSET >> 4)),     /* oszlop felso nibble */
        };
        if (i2c_master_transmit(s_dev, hdr, sizeof(hdr), -1) != ESP_OK) return;

        /* control 0x40 + 128 adatbajt egyben (NEM bajtonkent). */
        uint8_t row[1 + OLED_W];
        row[0] = OLED_CTRL_DATA;
        memcpy(&row[1], &s_fb[(uint16_t)page * OLED_W], OLED_W);
        if (i2c_master_transmit(s_dev, row, sizeof(row), -1) != ESP_OK) return;
    }
}

void display_oled_on(void)  { oled_cmd(0xAF); }
void display_oled_off(void) { oled_cmd(0xAE); }

/* ===================================================================== */
/* Rajz-primitívek (NX80 ssd1306.c:88-116 — változatlan logika)          */
/* ===================================================================== */

void display_oled_pixel(uint8_t x, uint8_t y, bool on)
{
    if (x >= OLED_W || y >= OLED_H) return;
    uint16_t idx = x + (uint16_t)(y >> 3) * OLED_W;
    uint8_t  m   = (uint8_t)(1u << (y & 7));
    if (on) s_fb[idx] |= m;
    else    s_fb[idx] &= (uint8_t)~m;
}

void display_oled_fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool on)
{
    for (uint8_t yy = 0; yy < h; yy++) {
        for (uint8_t xx = 0; xx < w; xx++) {
            display_oled_pixel((uint8_t)(x + xx), (uint8_t)(y + yy), on);
        }
    }
}

void display_oled_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    for (uint8_t xx = 0; xx < w; xx++) {
        display_oled_pixel((uint8_t)(x + xx), y, true);
        display_oled_pixel((uint8_t)(x + xx), (uint8_t)(y + h - 1), true);
    }
    for (uint8_t yy = 0; yy < h; yy++) {
        display_oled_pixel(x, (uint8_t)(y + yy), true);
        display_oled_pixel((uint8_t)(x + w - 1), (uint8_t)(y + yy), true);
    }
}

/* ===================================================================== */
/* Szövegrajz — 5x7 ASCII font (NX80 display.c:9-51 — változatlan logika) */
/* ===================================================================== */

void display_oled_char(uint8_t x, uint8_t y, char c, uint8_t scale, bool on)
{
    const uint8_t *g = oled_font_glyph(c);
    for (uint8_t col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (uint8_t row = 0; row < 7; row++) {
            if (bits & (1u << row)) {
                if (scale == 1) {
                    display_oled_pixel((uint8_t)(x + col), (uint8_t)(y + row), on);
                } else {
                    display_oled_fill_rect((uint8_t)(x + col * scale),
                                           (uint8_t)(y + row * scale),
                                           scale, scale, on);
                }
            }
        }
    }
}

uint8_t display_oled_text_width(const char *s, uint8_t scale)
{
    uint8_t n = 0;
    while (s[n]) n++;
    if (n == 0) return 0;
    /* karakterenkent 5px + 1px koz, az utolso koz nelkul */
    return (uint8_t)(n * 6 * scale - scale);
}

void display_oled_text(uint8_t x, uint8_t y, const char *s, uint8_t scale)
{
    while (*s) {
        display_oled_char(x, y, *s, scale, true);
        x = (uint8_t)(x + 6 * scale);
        s++;
    }
}

void display_oled_text_center(uint8_t y, const char *s, uint8_t scale)
{
    uint8_t w = display_oled_text_width(s, scale);
    uint8_t x = (w < OLED_W) ? (uint8_t)((OLED_W - w) / 2) : 0;
    display_oled_text(x, y, s, scale);
}

/* ===================================================================== */
/* Nagy szám-font — Swis721 19x24 (NX80 mikrofont.c — változatlan logika) */
/* ===================================================================== */

/* A font 24px magas -> 3 bajt/oszlop. */
#define OLED_BIGFONT_BYTES_HIGH  ((OLED_BIGFONT_HEIGHT + 7) / 8)

uint8_t display_oled_big_char(uint8_t x, uint8_t y, char c)
{
    if (c < OLED_BIGFONT_START || c > OLED_BIGFONT_END) c = OLED_BIGFONT_START;

    const uint8_t bh = OLED_BIGFONT_BYTES_HIGH;
    uint16_t bytes_per_char = (uint16_t)OLED_BIGFONT_WIDTH * bh + 1;  /* +1: szelesseg-bajt */
    const uint8_t *p =
        OLED_FONT_SWIS721_19x24 + (uint16_t)(c - OLED_BIGFONT_START) * bytes_per_char;

    uint8_t var_width = *p;     /* fejléc: változó szélesség */
    p++;

    for (uint8_t i = 0; i < var_width; i++) {
        for (uint8_t j = 0; j < bh; j++) {
            uint8_t dat = p[i * bh + j];
            for (uint8_t bit = 0; bit < 8; bit++) {
                if ((uint8_t)(j * 8 + bit) >= OLED_BIGFONT_HEIGHT) break;
                if (dat & (1u << bit)) {
                    display_oled_pixel((uint8_t)(x + i), (uint8_t)(y + j * 8 + bit), true);
                }
            }
        }
    }
    return var_width;
}

void display_oled_big_str(uint8_t x, uint8_t y, const char *s)
{
    while (*s) {
        uint8_t w = display_oled_big_char(x, y, *s);
        x = (uint8_t)(x + w + 1);   /* 1px koz a karakterek kozott */
        s++;
    }
}

uint8_t display_oled_big_str_width(const char *s)
{
    const uint8_t bh = OLED_BIGFONT_BYTES_HIGH;
    uint16_t bytes_per_char = (uint16_t)OLED_BIGFONT_WIDTH * bh + 1;
    uint16_t w = 0;
    for (; *s; s++) {
        char c = *s;
        if (c < OLED_BIGFONT_START || c > OLED_BIGFONT_END) c = OLED_BIGFONT_START;
        const uint8_t *p =
            OLED_FONT_SWIS721_19x24 + (uint16_t)(c - OLED_BIGFONT_START) * bytes_per_char;
        w += (uint16_t)(*p) + 1;
    }
    if (w > 0) w--;             /* utolso koz nem szamit */
    return (w > 255) ? 255 : (uint8_t)w;
}

void display_oled_big_center(uint8_t y, const char *s)
{
    uint8_t w = display_oled_big_str_width(s);
    uint8_t x = (w < OLED_W) ? (uint8_t)((OLED_W - w) / 2) : 0;
    display_oled_big_str(x, y, s);
}

/* ===================================================================== */
/* Demó / teszt rajz (vizuális ellenőrzéshez init után)                  */
/* ===================================================================== */

static void oled_demo_draw(void)
{
    display_oled_clear();

    /* Cím felül, középre, scale 1. */
    display_oled_text_center(0, "SWD PROGRAMMER", 1);

    /* Nagy szám-fonttal egy minta érték (százalék jellegű). */
    display_oled_big_str(2, 12, "100");

    /* Progress-bar keret alul + félig kitöltve. */
    const uint8_t bx = 2, by = 50, bw = 124, bh = 12;
    display_oled_rect(bx, by, bw, bh);
    display_oled_fill_rect((uint8_t)(bx + 2), (uint8_t)(by + 2),
                           (uint8_t)((bw - 4) / 2), (uint8_t)(bh - 4), true);

    display_oled_flush();
}

/* ===================================================================== */
/* Publikus init                                                         */
/* ===================================================================== */

esp_err_t display_oled_init(void)
{
    esp_err_t err;

    /* 1) I2C busz létrehozása (új i2c_master API). */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = OLED_I2C_PORT,
        .sda_io_num = OLED_I2C_SDA_GPIO,
        .scl_io_num = OLED_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* külső felhúzó ajánlott! */
    };
    err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus -> %s", esp_err_to_name(err));
        return err;
    }

    /* 2) Eszköz (OLED) hozzáadása a buszhoz. */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = OLED_I2C_ADDR,
        .scl_speed_hz    = OLED_I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device -> %s", esp_err_to_name(err));
        return err;
    }

    /* 3) Panel init. */
    err = oled_panel_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init -> %s", esp_err_to_name(err));
        return err;
    }

    /* 4) Clear + flush, majd boot felirat. */
    display_oled_clear();
    display_oled_flush();

    display_oled_clear();
    display_oled_text_center(28, "BOOT", 2);
    display_oled_flush();

    /* 5) Demó/teszt rajz a vizuális ellenőrzéshez. */
    oled_demo_draw();

    ESP_LOGI(TAG, "OLED init kesz (addr 0x%02X, SDA=%d SCL=%d, %d Hz, offset=%d, flip=%d)",
             OLED_I2C_ADDR, OLED_I2C_SDA_GPIO, OLED_I2C_SCL_GPIO,
             OLED_I2C_HZ, OLED_COL_OFFSET, OLED_FLIP_180);
    return ESP_OK;
}
