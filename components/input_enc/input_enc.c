/*
 * input_enc — Gombos enkóder bevitel (ESP32-S3, ESP-IDF v5.5.1)
 *
 * Enkóder:  PCNT periféria hardveres kvadratúra-dekóddal + glitch-filterrel
 *           (terv 14.1). Két PCNT-csatorna a teljes 4x dekódhoz; egy periodikus
 *           esp_timer pollozza a számlálót, és detent-enként ENC_CW/ENC_CCW
 *           eseményt tesz a queue-ba.
 * Gomb:     GPIO bemenet belső pullup-pal (aktív alacsony), esp_timer-es
 *           pollozott debounce, rövid vs hosszú nyomás megkülönböztetés.
 *
 * A debounce/short-long logika a NX80TESTER_AVR/button.c poll-alapú
 * állapotgépének portja (PORTA.IN -> gpio_get_level, millis() -> esp_timer).
 */
#include "input_enc.h"

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* ------------------------------------------------------------------ */
/* Konfiguráció (lábak és időzítés — terv 2.2 / 14. szekció)          */
/* ------------------------------------------------------------------ */
#define ENC_GPIO_A           10      /* enkóder A csatorna */
#define ENC_GPIO_B           11      /* enkóder B csatorna */
#define BTN_GPIO_SW          12      /* tengely-nyomógomb (GND-re húz) */

/* Mechanikus enkóder él/detent osztása. A tipikus enkóder 4 count/detent.
   Ha az enkóder „dupláz" vagy „felez", ezt kell állítani (pl. 2 vagy 1). */
#define ENC_COUNTS_PER_DETENT  4

/* PCNT glitch-filter (~1 us): a határérték ns-ben; a számláló él rövidebb
   tüskéit eldobja. */
#define ENC_GLITCH_NS        1000

/* PCNT számláló limitek. A poll gyakorisága mellett ez bőven elég;
   túlcsordulásnál a HW nullázza a hardveres számlálót, mi pedig delta-t
   képezünk, ezért a tényleges limit nem kritikus. */
#define PCNT_HIGH_LIMIT      1000
#define PCNT_LOW_LIMIT      (-1000)

/* Enkóder-poll periódus (terv: 5–10 ms). */
#define ENC_POLL_MS          5

/* Gomb debounce és nyomáshossz-küszöb (ms). */
#define BTN_DEBOUNCE_MS      15      /* stabil szint küszöb (terv: 5–20 ms) */
#define BTN_LONG_MS          600     /* e fölött: hosszú nyomás (terv: ~600 ms) */
#define BTN_POLL_MS          5       /* gomb-poll periódus */

/* Esemény-queue mérete. */
#define ENC_QUEUE_LEN        16

static const char *TAG = "input_enc";

/* ------------------------------------------------------------------ */
/* Modul-állapot                                                       */
/* ------------------------------------------------------------------ */
static QueueHandle_t    s_evq;          /* enc_event_t események sora */

/* Enkóder */
static pcnt_unit_handle_t s_pcnt;
static esp_timer_handle_t s_enc_timer;
static int                s_enc_accum;  /* maradék count (detent alá) */

/* Gomb (a button.c poll-állapotgép portja) */
static esp_timer_handle_t s_btn_timer;
static bool     s_stable_down  = false; /* debounce-olt állapot (lenyomva?) */
static bool     s_raw_prev     = false; /* előző nyers minta */
static int64_t  s_deb_t_us     = 0;     /* utolsó nyers-váltás ideje (us) */
static int64_t  s_press_us     = 0;     /* lenyomás kezdete (us) */
static bool     s_long_fired   = false; /* a hosszú-esemény már kiment? */

/* ------------------------------------------------------------------ */
/* Segéd: esemény a sorba (sosem blokkol, taszk- és ISR-mentes kontext)*/
/* ------------------------------------------------------------------ */
static inline void post_event(enc_event_t ev)
{
    /* esp_timer callback taszk-kontextusban fut, így a sima xQueueSend jó. */
    (void)xQueueSend(s_evq, &ev, 0);
}

/* ------------------------------------------------------------------ */
/* Enkóder-poll: PCNT delta -> detent -> ENC_CW/ENC_CCW                */
/* ------------------------------------------------------------------ */
static void enc_timer_cb(void *arg)
{
    (void)arg;
    int count = 0;
    if (pcnt_unit_get_count(s_pcnt, &count) != ESP_OK) {
        return;
    }
    /* Delta-t képzünk az előző leolvasáshoz képest, majd a hardveres
       számlálót nullázzuk (mint az AVR encoder_read()). */
    if (count != 0) {
        pcnt_unit_clear_count(s_pcnt);
    }

    s_enc_accum += count;

    /* Detent-enkénti leosztás: minden teljes ENC_COUNTS_PER_DETENT lépésre
       egy esemény, az előjelnek megfelelő irányba. A maradék megmarad. */
    while (s_enc_accum >= ENC_COUNTS_PER_DETENT) {
        s_enc_accum -= ENC_COUNTS_PER_DETENT;
        post_event(ENC_CW);
    }
    while (s_enc_accum <= -ENC_COUNTS_PER_DETENT) {
        s_enc_accum += ENC_COUNTS_PER_DETENT;
        post_event(ENC_CCW);
    }
}

static esp_err_t encoder_setup(void)
{
    /* PCNT egység: kvadratúra-dekódhoz mindkét csatorna él+szint akciókkal. */
    pcnt_unit_config_t unit_cfg = {
        .high_limit = PCNT_HIGH_LIMIT,
        .low_limit  = PCNT_LOW_LIMIT,
    };
    esp_err_t err = pcnt_new_unit(&unit_cfg, &s_pcnt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pcnt_new_unit: %s", esp_err_to_name(err));
        return err;
    }

    /* Glitch-filter (~1 us) a pergés/tüskék kiszűrésére. */
    pcnt_glitch_filter_config_t gf = { .max_glitch_ns = ENC_GLITCH_NS };
    err = pcnt_unit_set_glitch_filter(s_pcnt, &gf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_glitch_filter: %s", esp_err_to_name(err));
        return err;
    }

    /* 1. csatorna: él=A, szint=B. */
    pcnt_chan_config_t cfg_a = {
        .edge_gpio_num  = ENC_GPIO_A,
        .level_gpio_num = ENC_GPIO_B,
    };
    pcnt_channel_handle_t ch_a = NULL;
    err = pcnt_new_channel(s_pcnt, &cfg_a, &ch_a);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pcnt_new_channel A: %s", esp_err_to_name(err));
        return err;
    }
    /* A él dekódja: felfutó +1, lefutó -1; B szint szerint irányváltás. */
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(ch_a,
        PCNT_CHANNEL_EDGE_ACTION_DECREASE,   /* negatív él */
        PCNT_CHANNEL_EDGE_ACTION_INCREASE)); /* pozitív él */
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(ch_a,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP,      /* B magas: tart */
        PCNT_CHANNEL_LEVEL_ACTION_INVERSE)); /* B alacsony: fordít */

    /* 2. csatorna: él=B, szint=A — a teljes 4x dekódhoz (mindkét él, mindkét cs.). */
    pcnt_chan_config_t cfg_b = {
        .edge_gpio_num  = ENC_GPIO_B,
        .level_gpio_num = ENC_GPIO_A,
    };
    pcnt_channel_handle_t ch_b = NULL;
    err = pcnt_new_channel(s_pcnt, &cfg_b, &ch_b);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pcnt_new_channel B: %s", esp_err_to_name(err));
        return err;
    }
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(ch_b,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE,   /* negatív él */
        PCNT_CHANNEL_EDGE_ACTION_DECREASE)); /* pozitív él */
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(ch_b,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP,
        PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    /* Egység engedélyezése + nullázás + indítás. */
    ESP_ERROR_CHECK(pcnt_unit_enable(s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_start(s_pcnt));

    /* Periodikus poll-timer. */
    const esp_timer_create_args_t targs = {
        .callback = enc_timer_cb,
        .name     = "enc_poll",
    };
    err = esp_timer_create(&targs, &s_enc_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create enc: %s", esp_err_to_name(err));
        return err;
    }
    return esp_timer_start_periodic(s_enc_timer, ENC_POLL_MS * 1000);
}

/* ------------------------------------------------------------------ */
/* Gomb-poll: debounce + short/long (button.c port)                    */
/* ------------------------------------------------------------------ */
static void btn_timer_cb(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();                 /* us */
    bool raw = (gpio_get_level(BTN_GPIO_SW) == 0);      /* aktív alacsony -> lenyomva */

    /* Nyers-váltás -> debounce-óra újraindítása. */
    if (raw != s_raw_prev) {
        s_raw_prev  = raw;
        s_deb_t_us  = now;
    }

    /* Stabil szint küszöb elérése után állapot-átmenet. */
    if ((now - s_deb_t_us) >= (int64_t)BTN_DEBOUNCE_MS * 1000 &&
        raw != s_stable_down) {
        s_stable_down = raw;
        if (s_stable_down) {
            /* lenyomás eleje */
            s_press_us   = now;
            s_long_fired = false;
        } else {
            /* felengedés: ha nem ment ki long, és a tartás rövid volt -> short */
            int64_t dur = now - s_press_us;
            if (!s_long_fired && dur < (int64_t)BTN_LONG_MS * 1000) {
                post_event(BTN_SHORT);
            }
        }
    }

    /* Nyomvatartás közben a küszöb elérésekor azonnal BTN_LONG. */
    if (s_stable_down && !s_long_fired) {
        if ((now - s_press_us) >= (int64_t)BTN_LONG_MS * 1000) {
            s_long_fired = true;
            post_event(BTN_LONG);
        }
    }
}

static esp_err_t button_setup(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BTN_GPIO_SW,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,   /* pollozott debounce, nincs ISR */
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config btn: %s", esp_err_to_name(err));
        return err;
    }

    /* Kezdeti állapot a tényleges szintből (ne generáljon hamis eseményt). */
    s_raw_prev    = (gpio_get_level(BTN_GPIO_SW) == 0);
    s_stable_down = s_raw_prev;
    s_deb_t_us    = esp_timer_get_time();

    const esp_timer_create_args_t targs = {
        .callback = btn_timer_cb,
        .name     = "btn_poll",
    };
    err = esp_timer_create(&targs, &s_btn_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create btn: %s", esp_err_to_name(err));
        return err;
    }
    return esp_timer_start_periodic(s_btn_timer, BTN_POLL_MS * 1000);
}

/* ------------------------------------------------------------------ */
/* Publikus API                                                        */
/* ------------------------------------------------------------------ */
esp_err_t input_enc_init(void)
{
    s_evq = xQueueCreate(ENC_QUEUE_LEN, sizeof(enc_event_t));
    if (s_evq == NULL) {
        ESP_LOGE(TAG, "queue alloc failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = encoder_setup();
    if (err != ESP_OK) {
        return err;
    }

    err = button_setup();
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "init kész: enc A=%d B=%d, gomb SW=%d",
             ENC_GPIO_A, ENC_GPIO_B, BTN_GPIO_SW);
    return ESP_OK;
}

bool input_enc_get(enc_event_t *ev, uint32_t ticks_to_wait)
{
    if (ev == NULL || s_evq == NULL) {
        return false;
    }
    return xQueueReceive(s_evq, ev, (TickType_t)ticks_to_wait) == pdTRUE;
}
