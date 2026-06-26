/*
 * ui.c — Helyi UI állapotgép (ui_task) az OLED + enkóder fölött.
 *
 * Terv 15. szekció: a taszk az input_enc eseménysorán blokkol
 * (input_enc_get portMAX_DELAY), és CSAK eseményre/változásra renderel
 * (terv 15.1). Az állapotot statikus változókban tartjuk; minden
 * eseménynél frissítjük, majd újrarajzolunk (clear -> rajz -> flush).
 *
 * Képernyő-flow (terv 15.x):
 *   Idle/status  --BTN_SHORT-->  Főmenü
 *   Főmenü       --BTN_SHORT-->  almenü (Program fw / placeholderek)
 *                --BTN_LONG -->  Idle
 *   Program fw   : /lfs/fw lista; BTN_SHORT -> "Selected" megerősítő
 *                --BTN_LONG -->  Főmenü
 *
 * UTF-8, magyar kommentek.
 */
#include "ui.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display_oled.h"
#include "input_enc.h"
#include "storage_lfs.h"

#include "esp_log.h"

static const char *TAG = "ui";

/* ---------------------------------------------------------------------- */
/* Layout-konstansok (5x7 font scale 1: 6px széles cella, 8px sormagasság) */
/* ---------------------------------------------------------------------- */
#define UI_LINE_H      10          /* egy listasor magassága px-ben       */
#define UI_HDR_H       11          /* fejléc-sáv magassága px-ben         */
#define UI_LIST_TOP    UI_HDR_H    /* az első listasor y-ja               */
#define UI_VISIBLE     5           /* egyszerre látható listaelemek száma */

/* ---------------------------------------------------------------------- */
/* Fájllista-puffer (statikus; max 32 név × 24 char) — terv 15.x          */
/* ---------------------------------------------------------------------- */
#define FW_MAX_FILES   32
#define FW_NAME_LEN    24

static char    s_fw_names[FW_MAX_FILES][FW_NAME_LEN];
static uint8_t s_fw_count;          /* érvényes nevek száma a pufferben   */

/* ---------------------------------------------------------------------- */
/* Állapotgép — aktuális képernyő + navigációs állapot (statikus)         */
/* ---------------------------------------------------------------------- */
typedef enum {
    SCR_IDLE,       /* állapot/status képernyő          */
    SCR_MENU,       /* főmenü                           */
    SCR_FWLIST,     /* /lfs/fw fájllista                */
    SCR_FWSEL,      /* kiválasztott fw megerősítő       */
    SCR_PLACEHOLDER /* "(hamarosan)" almenük            */
} ui_screen_t;

static ui_screen_t s_screen   = SCR_IDLE;
static int         s_sel      = 0;   /* kiválasztott elem indexe          */
static int         s_scroll   = 0;   /* scroll-offset (első látható elem) */
static char        s_sel_name[FW_NAME_LEN]; /* kiválasztott fw neve       */
static const char *s_ph_title = "";  /* aktuális placeholder címe         */

/* Főmenü elemei (terv 15.2). */
static const char *const MENU_ITEMS[] = {
    "Program firmware",
    "Cel konfig",
    "Elo adat",
    "Beallitasok",
};
#define MENU_COUNT ((int)(sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0])))

/* ---------------------------------------------------------------------- */
/* Rajz-segédek                                                           */
/* ---------------------------------------------------------------------- */

/* Inverz fejléc-sáv: kitöltött téglalap + sötét (on=false) szöveg rajta. */
static void ui_draw_header(const char *title)
{
    display_oled_fill_rect(0, 0, OLED_W, UI_HDR_H, true);
    /* Cím balra igazítva, 2px margóval, scale 1, sötét (on=false) pixelekkel. */
    for (int i = 0; title[i] && i < 21; i++) {
        display_oled_char(2 + i * 6, 2, title[i], 1, false);
    }
}

/* Általános görgetett listarajzoló:
 *   - fejléc (inverz sáv),
 *   - legfeljebb UI_VISIBLE elem a scroll-offsettől,
 *   - a kiválasztott elem keretes/inverz kiemeléssel.
 * A 'get_item' callback adja vissza az i. elem szövegét. */
typedef const char *(*ui_item_getter)(int idx);

static void ui_draw_list(const char *title, int count, int sel, int scroll,
                         ui_item_getter get_item, const char *empty_msg)
{
    display_oled_clear();
    ui_draw_header(title);

    if (count <= 0) {
        display_oled_text_center(UI_LIST_TOP + UI_LINE_H, empty_msg, 1);
        display_oled_flush();
        return;
    }

    for (int row = 0; row < UI_VISIBLE; row++) {
        int idx = scroll + row;
        if (idx >= count) break;

        int y = UI_LIST_TOP + row * UI_LINE_H;
        const char *txt = get_item(idx);

        if (idx == sel) {
            /* Kiemelés: kitöltött sáv + sötét szöveg (inverz). */
            display_oled_fill_rect(0, y - 1, OLED_W, UI_LINE_H, true);
            for (int i = 0; txt[i] && i < 21; i++) {
                display_oled_char(2 + i * 6, y, txt[i], 1, false);
            }
        } else {
            display_oled_text(2, y, txt, 1);
        }
    }

    display_oled_flush();
}

/* Item-getterek a két listához. */
static const char *menu_item(int idx)   { return MENU_ITEMS[idx]; }
static const char *fw_item(int idx)     { return s_fw_names[idx]; }

/* ---------------------------------------------------------------------- */
/* Fájllista-gyűjtés a /lfs/fw könyvtárból (lock alatt) — terv 15.x       */
/* ---------------------------------------------------------------------- */

/* A listázó callback: csak közönséges fájlokat veszünk fel a pufferbe. */
static void fw_collect_cb(const char *name, size_t size, bool is_dir, void *ctx)
{
    (void)size; (void)ctx;
    if (is_dir) return;
    if (s_fw_count >= FW_MAX_FILES) return;
    strncpy(s_fw_names[s_fw_count], name, FW_NAME_LEN - 1);
    s_fw_names[s_fw_count][FW_NAME_LEN - 1] = '\0';
    s_fw_count++;
}

/* A teljes gyűjtést a közös lock alatt végezzük (terv 11. + 15.x). */
static void fw_list_load(void)
{
    s_fw_count = 0;
    storage_lfs_lock();
    esp_err_t err = storage_lfs_list(STORAGE_LFS_BASE "/fw", fw_collect_cb, NULL);
    storage_lfs_unlock();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fw lista hiba: %s", esp_err_to_name(err));
        /* hibánál üres listát mutatunk */
    }
}

/* ---------------------------------------------------------------------- */
/* Képernyő-rajzolók                                                      */
/* ---------------------------------------------------------------------- */

/* 1. Idle/status: cím + állapotsorok (egyelőre placeholder értékek). */
static void draw_idle(void)
{
    display_oled_clear();
    display_oled_text_center(0, "SWD PROG", 2);
    display_oled_text(2, 26, "WiFi:   --", 1);
    display_oled_text(2, 38, "Target: --", 1);
    display_oled_text(2, 50, "Serial: --", 1);
    display_oled_flush();
}

/* 4. Placeholder almenük. */
static void draw_placeholder(void)
{
    display_oled_clear();
    ui_draw_header(s_ph_title);
    display_oled_text_center(28, "(hamarosan)", 1);
    display_oled_flush();
}

/* 3b. Kiválasztott fw megerősítő képernyő. */
static void draw_fwsel(void)
{
    display_oled_clear();
    ui_draw_header("Program firmware");
    display_oled_text(2, 18, "Selected:", 1);
    display_oled_text(2, 30, s_sel_name, 1);
    display_oled_text(2, 46, "Flash: TODO", 1);
    display_oled_flush();
}

/* A teljes aktuális képernyő kirajzolása az állapot alapján. */
static void ui_render(void)
{
    switch (s_screen) {
    case SCR_IDLE:
        draw_idle();
        break;
    case SCR_MENU:
        ui_draw_list("Fomenu", MENU_COUNT, s_sel, s_scroll, menu_item, "");
        break;
    case SCR_FWLIST:
        ui_draw_list("Program firmware", s_fw_count, s_sel, s_scroll,
                     fw_item, "(nincs fw fajl)");
        break;
    case SCR_FWSEL:
        draw_fwsel();
        break;
    case SCR_PLACEHOLDER:
        draw_placeholder();
        break;
    }
}

/* ---------------------------------------------------------------------- */
/* Navigáció-segédek                                                      */
/* ---------------------------------------------------------------------- */

/* Görgetés egy listában: a kijelölést mozgatja és a scroll-ablakot
   úgy igazítja, hogy a kiválasztott elem mindig látható maradjon. */
static void list_move(int delta, int count)
{
    if (count <= 0) return;
    s_sel += delta;
    if (s_sel < 0)        s_sel = 0;
    if (s_sel >= count)   s_sel = count - 1;

    if (s_sel < s_scroll)                    s_scroll = s_sel;
    if (s_sel >= s_scroll + UI_VISIBLE)      s_scroll = s_sel - UI_VISIBLE + 1;
}

/* Belépés egy listába/almenübe: index + scroll nullázás. */
static void enter_screen(ui_screen_t scr)
{
    s_screen = scr;
    s_sel    = 0;
    s_scroll = 0;
}

/* ---------------------------------------------------------------------- */
/* Eseménykezelő — esemény + jelenlegi állapot -> új állapot             */
/* ---------------------------------------------------------------------- */
static void ui_handle(enc_event_t ev)
{
    switch (s_screen) {

    /* --- Idle/status --- */
    case SCR_IDLE:
        if (ev == BTN_SHORT) {
            enter_screen(SCR_MENU);
        }
        break;

    /* --- Főmenü --- */
    case SCR_MENU:
        switch (ev) {
        case ENC_CW:    list_move(+1, MENU_COUNT); break;
        case ENC_CCW:   list_move(-1, MENU_COUNT); break;
        case BTN_LONG:  enter_screen(SCR_IDLE);    break;
        case BTN_SHORT:
            if (s_sel == 0) {            /* Program firmware */
                fw_list_load();
                enter_screen(SCR_FWLIST);
            } else {                     /* placeholder almenük */
                s_ph_title = MENU_ITEMS[s_sel];
                enter_screen(SCR_PLACEHOLDER);
            }
            break;
        }
        break;

    /* --- Program firmware lista --- */
    case SCR_FWLIST:
        switch (ev) {
        case ENC_CW:    list_move(+1, s_fw_count); break;
        case ENC_CCW:   list_move(-1, s_fw_count); break;
        case BTN_LONG:  enter_screen(SCR_MENU);    break;
        case BTN_SHORT:
            if (s_fw_count > 0) {
                strncpy(s_sel_name, s_fw_names[s_sel], FW_NAME_LEN - 1);
                s_sel_name[FW_NAME_LEN - 1] = '\0';
                s_screen = SCR_FWSEL;
            }
            break;
        }
        break;

    /* --- Kiválasztott fw megerősítő --- */
    case SCR_FWSEL:
        if (ev == BTN_LONG || ev == BTN_SHORT) {
            /* Vissza a fájllistához (a flashelés még nincs bekötve). */
            s_screen = SCR_FWLIST;
        }
        break;

    /* --- Placeholder almenük --- */
    case SCR_PLACEHOLDER:
        if (ev == BTN_LONG) {
            enter_screen(SCR_MENU);
        }
        break;
    }
}

/* ---------------------------------------------------------------------- */
/* A taszk: eseménysoron blokkol, csak változásra renderel (terv 15.1)    */
/* ---------------------------------------------------------------------- */
static void ui_task(void *arg)
{
    (void)arg;

    /* Kezdő képernyő kirajzolása egyszer. */
    ui_render();

    enc_event_t ev;
    for (;;) {
        if (input_enc_get(&ev, portMAX_DELAY)) {
            ui_handle(ev);   /* állapot frissítése */
            ui_render();     /* clear -> rajz -> flush */
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Publikus API                                                           */
/* ---------------------------------------------------------------------- */
esp_err_t ui_start(void)
{
    BaseType_t ok = xTaskCreate(ui_task, "ui_task", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "ui_task letrehozas sikertelen");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
