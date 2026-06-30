/*
 * display_lcd.c — ILI9488 480x320 SPI + GT911 touch + LVGL v9 bring-up (D1+D2).
 *
 * !!! HW-n MÉG NEM IGAZOLT — D1/D2/D4 HW-teszt hátravan. !!!
 * A kód zöldre fordul, de valódi panel/touch hiányában a szín, orientáció,
 * SPI-frekvencia és a GT911 I2C-cím-szekvencia nincs validálva. Az orientáció
 * (swap_xy/mirror) és a pclk a HW-bring-up alatt hangolandó — a releváns
 * helyeken "HW-n hangolandó" megjegyzés jelzi.
 *
 * Rétegek:
 *   1) SPI2 (FSPI) busz   — MOSI=8, SCLK=9, max_transfer a draw-bufferhez méretezve
 *   2) esp_lcd panel IO   — CS=38, DC=39, SPI @ 40 MHz (biztonságos indulás)
 *   3) ILI9488 panel      — RST=40, bits_per_pixel=18 (RGB666; a driver konvertál)
 *   4) BL (háttérvilágítás)— GPIO41 kimenet, bekapcsolva
 *   5) LVGL-port          — lvgl_port_init + add_disp (internal DMA draw-buffer)
 *   6) GT911 touch        — külön I2C master bus (SDA=47, SCL=48), INT=42, RST=2
 *   7) Touch-indev        — lvgl_port_add_touch (pointer)
 *   8) Enkóder-indev      — EGYEDI LV_INDEV_TYPE_ENCODER, input_enc queue-ról
 *
 * Az enkóder szándékosan NEM a lvgl_port_add_encoder/knob (az PCNT/GPIO-t
 * újrainicializálná a 10/11/12-n, ütközne az input_enc-cel). Helyette saját,
 * nem-blokkoló read_cb üríti az input_enc_get() queue-t és fordít LVGL-eseményre.
 */
#include "display_lcd.h"
#include "input_enc.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_ili9488.h"
#include "esp_lcd_touch_gt911.h"

static const char *TAG = "display_lcd";

/* ------------------------------------------------------------------ */
/* Lábkiosztás (terv 1. szekció) — könnyen hangolható #define-ok       */
/* ------------------------------------------------------------------ */
/* LCD SPI2/FSPI (MISO nincs — write-only panel). */
#define LCD_SPI_HOST        SPI2_HOST
#define LCD_GPIO_SCLK       9
#define LCD_GPIO_MOSI       8
#define LCD_GPIO_CS         38
#define LCD_GPIO_DC         39
#define LCD_GPIO_RST        40
#define LCD_GPIO_BL         41
/* SPI órajel. 40 MHz biztonságos indulás; HW-mérés után feljebb (max ~80 MHz). */
#define LCD_PCLK_HZ         (40 * 1000 * 1000)

/* Panel felbontás (480x320 fekvő). */
#define LCD_H_RES           480
#define LCD_V_RES           320
#define LCD_BITS_PER_PIXEL  18      /* RGB666 — SPI-módban KÖTELEZŐ az ILI9488-nál */

/* GT911 touch I2C (új master bus, I2C_NUM_0). */
#define TP_I2C_PORT         I2C_NUM_0
#define TP_GPIO_SDA         47
#define TP_GPIO_SCL         48
#define TP_GPIO_INT         42
#define TP_GPIO_RST         2
#define TP_I2C_HZ           400000

/* ------------------------------------------------------------------ */
/* Draw-buffer méretezés                                               */
/* ------------------------------------------------------------------ */
/* A reference 3./5. szekció szerint a draw-buffer INTERNAL DMA-RAM-ba kerül
   (ne PSRAM), hogy a flash alatt a PSRAM<->SPI DMA-kontenció ne glitch-elje a
   bit-bang SWD-t. 40 sor x 480 px x 2 bájt (RGB565, LVGL belül 16-bit) =
   ~37.5 KB/puffer; dupla puffer => ~75 KB internal DMA-RAM. Ez kényelmesen
   befér az S3 internal SRAM-jába. (Ha szűkülne, csökkentsd a sor-számot.) */
#define LCD_DRAW_BUF_LINES  40
#define LCD_DRAW_BUF_PX     (LCD_H_RES * LCD_DRAW_BUF_LINES)

/* ------------------------------------------------------------------ */
/* Modul-állapot                                                       */
/* ------------------------------------------------------------------ */
static esp_lcd_panel_io_handle_t   s_io_handle    = NULL;
static esp_lcd_panel_handle_t      s_panel_handle = NULL;
static i2c_master_bus_handle_t     s_tp_bus       = NULL;
static esp_lcd_panel_io_handle_t   s_tp_io_handle = NULL;
static esp_lcd_touch_handle_t      s_tp_handle    = NULL;
static lv_display_t               *s_disp         = NULL;
static lv_indev_t                 *s_indev_touch  = NULL;
static lv_indev_t                 *s_indev_enc    = NULL;
static lv_group_t                 *s_group        = NULL;

/* A "vissza" (BTN_LONG) callback. Az indev read_cb-ből hívjuk (port-taszk). */
static void (*s_back_cb)(void) = NULL;

/* ------------------------------------------------------------------ */
/* Enkóder-indev read_cb (input_enc queue -> LVGL encoder)             */
/* ------------------------------------------------------------------ */
/* Nem-blokkoló: egy hívásban EGY eseményt vesz ki a queue-ból (ticks=0).
 * Leképezés:
 *   ENC_CW   -> enc_diff = +1
 *   ENC_CCW  -> enc_diff = -1
 *   BTN_SHORT-> state PRESSED (a következő hívásban RELEASED) = LVGL aktiválás
 *   BTN_LONG -> s_back_cb() (nincs LVGL-encoder "back" fogalom)
 *
 * FIGYELEM: az input_enc queue-nak EGYETLEN fogyasztója lehet — mostantól ez a
 * read_cb. A régi ui.c (OLED) még a queue-t olvassa; azt a D3 írja át. Ez a
 * fájl a ui.c-t NEM módosítja és NEM köti be magát a main-be (OLED-build zöld).
 */
static void enc_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    /* Alapból nincs forgás. */
    data->enc_diff = 0;

    /* Ha az előző hívásban PRESSED-et jeleztünk, most engedjük el. Egy esemény
       / hívás elv: nem szedünk ki több eseményt egyszerre, így az LVGL minden
       léptetést/kattintást külön frame-ben lát. */
    static bool pressed = false;
    if (pressed) {
        data->state = LV_INDEV_STATE_RELEASED;
        pressed = false;
        return;
    }

    enc_event_t ev;
    if (!input_enc_get(&ev, 0)) {
        /* Nincs esemény — fenntartjuk a released állapotot. */
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    switch (ev) {
    case ENC_CW:
        data->enc_diff = 1;
        data->state = LV_INDEV_STATE_RELEASED;
        break;
    case ENC_CCW:
        data->enc_diff = -1;
        data->state = LV_INDEV_STATE_RELEASED;
        break;
    case BTN_SHORT:
        /* PRESSED most, RELEASED a következő hívásban -> LVGL "click"/aktiválás. */
        data->state = LV_INDEV_STATE_PRESSED;
        pressed = true;
        break;
    case BTN_LONG:
        /* "Vissza" — saját callback (port-taszk kontextus). */
        data->state = LV_INDEV_STATE_RELEASED;
        if (s_back_cb) {
            s_back_cb();
        }
        break;
    default:
        data->state = LV_INDEV_STATE_RELEASED;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Háttérvilágítás (BL) — egyszerű GPIO ki/be                          */
/* ------------------------------------------------------------------ */
static esp_err_t lcd_backlight_init(bool on)
{
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << LCD_GPIO_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&bl_cfg);
    if (err != ESP_OK) {
        return err;
    }
    /* Aktív-magas BL feltételezve (HW-n hangolandó, ha fordított). */
    return gpio_set_level(LCD_GPIO_BL, on ? 1 : 0);
}

/* ------------------------------------------------------------------ */
/* Panel bring-up (D1)                                                 */
/* ------------------------------------------------------------------ */
static esp_err_t panel_bringup(void)
{
    esp_err_t err;

    /* 1) SPI busz. max_transfer_sz a legnagyobb egyszerre küldött adathoz:
          a draw-buffer pixelei * 3 bájt (RGB666 a panel felé). */
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = LCD_GPIO_SCLK,
        .mosi_io_num = LCD_GPIO_MOSI,
        .miso_io_num = -1,                 /* write-only panel */
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_DRAW_BUF_PX * 3,
    };
    err = spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize -> %s", esp_err_to_name(err));
        return err;
    }

    /* 2) esp_lcd panel IO (SPI). DC=39, CS=38, 40 MHz. ILI9488: 8-bites
          parancs/param (lcd_cmd_bits/lcd_param_bits = 8). */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_GPIO_DC,
        .cs_gpio_num = LCD_GPIO_CS,
        .pclk_hz = LCD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    err = esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_cfg, &s_io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi -> %s", esp_err_to_name(err));
        return err;
    }

    /* 3) ILI9488 panel. RST=40, bits_per_pixel=18 (RGB666 — KÖTELEZŐ SPI-n).
          A buffer_size a driver belső RGB565->RGB666 konverziós pufferéhez kell;
          a draw-buffer egy darabjára méretezzük. */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_GPIO_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,  /* HW-n hangolandó: RGB/BGR */
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
    };
    err = esp_lcd_new_panel_ili9488(s_io_handle, &panel_cfg, LCD_DRAW_BUF_PX,
                                    &s_panel_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_ili9488 -> %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_reset(s_panel_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel_reset -> %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_init(s_panel_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel_init -> %s", esp_err_to_name(err));
        return err;
    }

    /* Orientáció — 480x320 FEKVŐ default. A natív ILI9488 320(x) x 480(y) álló;
       fekvőhöz swap_xy + mirror. EZEK A FLAG-EK HW-n HANGOLANDÓK (a kép
       irányát/tükrözését valódi panelen kell ellenőrizni). */
    esp_lcd_panel_swap_xy(s_panel_handle, true);
    esp_lcd_panel_mirror(s_panel_handle, true, false);
    /* Szín-inverzió: ILI9488-nál tipikusan szükségtelen; HW-n hangolandó. */
    esp_lcd_panel_invert_color(s_panel_handle, false);
    /* Bekapcsolás (egyes esp_lcd verziók: alapból ki). */
    esp_lcd_panel_disp_on_off(s_panel_handle, true);

    /* 4) Háttérvilágítás be. */
    err = lcd_backlight_init(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight_init -> %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* LVGL-port + display (D1)                                            */
/* ------------------------------------------------------------------ */
static esp_err_t lvgl_disp_bringup(void)
{
    /* LVGL-port (render-taszk + tick-timer + lock). */
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    esp_err_t err = lvgl_port_init(&port_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init -> %s", esp_err_to_name(err));
        return err;
    }

    /* Display hozzáadása. Draw-buffer INTERNAL DMA-RAM-ban (buff_dma=1,
       buff_spiram=0) — flash-alatti PSRAM-kontenció elkerülése (reference 5.).
       Dupla puffer a sima rajzhoz. A panel BGR/szín-konverzió a driverben. */
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io_handle,
        .panel_handle = s_panel_handle,
        .buffer_size = LCD_DRAW_BUF_PX,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,  /* LVGL belül 16-bit */
        .rotation = {
            /* A HW-orientációt a panelen (swap_xy/mirror) állítjuk, nem itt;
               itt minden false (SW-rotációt nem használunk). */
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,      /* internal DMA-capable buffer */
            .buff_spiram = false,  /* NE PSRAM (flash-jitter elkerülése) */
            .swap_bytes = false,   /* HW-n hangolandó, ha a színek byte-cseréltek */
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp -> NULL");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* GT911 touch + touch-indev (D2)                                      */
/* ------------------------------------------------------------------ */
static esp_err_t touch_bringup(void)
{
    /* Külön I2C master bus a touch-hoz (a panel SPI-n megy). */
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = TP_I2C_PORT,
        .sda_io_num = TP_GPIO_SDA,
        .scl_io_num = TP_GPIO_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = true },
    };
    esp_err_t err = i2c_new_master_bus(&i2c_cfg, &s_tp_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus(touch) -> %s", esp_err_to_name(err));
        return err;
    }

    /* GT911 panel IO az I2C-buszon. */
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_cfg.scl_speed_hz = TP_I2C_HZ;
    err = esp_lcd_new_panel_io_i2c(s_tp_bus, &tp_io_cfg, &s_tp_io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_i2c(touch) -> %s", esp_err_to_name(err));
        return err;
    }

    /* GT911 touch driver. INT=42, RST=2. A koordináta-orientációt a kijelzőhöz
       igazítjuk (swap_xy + mirror_x), hogy a touch a fekvő panellel egyezzen —
       HW-n hangolandó. Az INT/RST szekvenciát (I2C-cím-választás) a driver intézi. */
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = TP_GPIO_RST,
        .int_gpio_num = TP_GPIO_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = true,    /* HW-n hangolandó (a panel orientációjához) */
            .mirror_x = true,   /* HW-n hangolandó */
            .mirror_y = false,  /* HW-n hangolandó */
        },
    };
    err = esp_lcd_touch_new_i2c_gt911(s_tp_io_handle, &tp_cfg, &s_tp_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_touch_new_i2c_gt911 -> %s", esp_err_to_name(err));
        return err;
    }

    /* Touch -> LVGL pointer-indev az esp_lvgl_port-on keresztül. */
    lvgl_port_touch_cfg_t touch_cfg = {
        .disp = s_disp,
        .handle = s_tp_handle,
    };
    s_indev_touch = lvgl_port_add_touch(&touch_cfg);
    if (s_indev_touch == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_touch -> NULL");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Enkóder-indev + navigációs csoport (D2)                             */
/* ------------------------------------------------------------------ */
static esp_err_t encoder_indev_bringup(void)
{
    /* Az LVGL-hívásokat a port-lock alatt végezzük (a port-taszkon kívül). */
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "lvgl_port_lock -> false (encoder indev)");
        return ESP_FAIL;
    }

    /* EGYEDI encoder-indev — NEM a port knob/encoder (az PCNT-t újrainicializálná). */
    s_indev_enc = lv_indev_create();
    if (s_indev_enc == NULL) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "lv_indev_create -> NULL");
        return ESP_FAIL;
    }
    lv_indev_set_type(s_indev_enc, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(s_indev_enc, enc_read_cb);
    lv_indev_set_display(s_indev_enc, s_disp);

    /* Navigációs csoport — a D3 ebbe veszi a fókuszálható widgeteket. */
    s_group = lv_group_create();
    if (s_group == NULL) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "lv_group_create -> NULL");
        return ESP_FAIL;
    }
    lv_group_set_default(s_group);            /* új widgetek auto-csoportja */
    lv_indev_set_group(s_indev_enc, s_group); /* az enkóder ezt vezérli */

    lvgl_port_unlock();
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Publikus API                                                        */
/* ------------------------------------------------------------------ */
esp_err_t display_lcd_init(void)
{
    ESP_LOGI(TAG, "display_lcd_init — ILI9488 + GT911 + LVGL %d.%d.%d "
             "(HW-n MÉG NEM IGAZOLT)",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);

    esp_err_t err;

    /* D1: panel. */
    err = panel_bringup();
    if (err != ESP_OK) {
        return err;
    }

    /* D1: LVGL-port + display. */
    err = lvgl_disp_bringup();
    if (err != ESP_OK) {
        return err;
    }

    /* D2: GT911 touch + pointer-indev. */
    err = touch_bringup();
    if (err != ESP_OK) {
        return err;
    }

    /* D2: enkóder-indev + navigációs csoport. */
    err = encoder_indev_bringup();
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "display_lcd_init OK (%dx%d, draw-buf %d sor x2, internal DMA)",
             LCD_H_RES, LCD_V_RES, LCD_DRAW_BUF_LINES);
    return ESP_OK;
}

lv_group_t *display_lcd_group(void)
{
    return s_group;
}

void display_lcd_set_back_cb(void (*cb)(void))
{
    s_back_cb = cb;
}
