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
#include "prog_session.h"   /* SWD flash orchestráció (prog_session_flash_file) */

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
    "Cel info (SWD)",
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
    display_oled_text(2, 46, "OK=flash", 1);   /* BTN_SHORT -> flash indul */
    display_oled_flush();
}

/* ---------------------------------------------------------------------- */
/* Flash progress — prog_session callback + indító (terv 8/15)            */
/* ---------------------------------------------------------------------- */

/* prog_phase_t -> rövid magyar fázis-címke a progress-képernyőhöz. */
static const char *phase_label(prog_phase_t p)
{
    switch (p) {
    case PROG_CONNECT: return "Connect";
    case PROG_ERASE:   return "Torles";
    case PROG_PROGRAM: return "Iras";
    case PROG_VERIFY:  return "Ellenor.";
    case PROG_DONE:    return "KESZ";
    case PROG_FAILED:  return "HIBA";
    default:           return "...";
    }
}

/* prog_session progress callback: a flash-elő (itt: ui_task) kontextusából
   hívódik minden fázis-/százalék-váltáskor. Egy teljes képernyőt rajzol:
   fázis-címke (fejléc), target_name és egy százalékos progress-bar.
   Minden hívásnál clear -> rajz -> flush (terv 15.1). */
static void ui_flash_cb(const prog_status_t *st, void *ctx)
{
    (void)ctx;
    if (!st) return;

    /* Százalék 0..100 közé szorítva (defenzív a bar-rajzhoz). */
    int pct = st->percent;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    display_oled_clear();
    ui_draw_header(phase_label(st->phase));

    /* Cél neve (vagy "?"), illetve a százalék szám szövegként. */
    display_oled_text(2, 18, st->target_name[0] ? st->target_name : "?", 1);

    char pbuf[8];
    snprintf(pbuf, sizeof(pbuf), "%d%%", pct);
    display_oled_text(2, 30, pbuf, 1);

    /* Progress-bar: 1px keret + belső kitöltés a százalék arányában. */
    const uint8_t bx = 2, by = 44, bw = OLED_W - 4, bh = 12;
    display_oled_rect(bx, by, bw, bh);
    int fill = ((bw - 2) * pct) / 100;
    if (fill > 0) {
        display_oled_fill_rect(bx + 1, by + 1, (uint8_t)fill, bh - 2, true);
    }

    display_oled_flush();
}

/* A kiválasztott fw szinkron flash-elése a ui_task kontextusából. A flash
   ideje alatt a UI nem dolgoz fel bevitelt (a prog_session szinkron) — ez
   flash közben elfogadható. A végén egy eredmény-képernyőt mutat, és egy
   gombnyomásig blokkol, majd a hívó visszaviszi a menübe. */
static void ui_start_flash(void)
{
    char fw_path[64];
    snprintf(fw_path, sizeof(fw_path), STORAGE_LFS_BASE "/fw/%s", s_sel_name);

    ESP_LOGI(TAG, "flash indul: %s", fw_path);
    esp_err_t err = prog_session_flash_file(fw_path, 0, ui_flash_cb, NULL);

    /* Eredmény-képernyő: OK -> "KESZ", hiba -> esp_err név (rövid). */
    display_oled_clear();
    if (err == ESP_OK) {
        ui_draw_header("KESZ");
        display_oled_text_center(28, "Flash OK", 1);
        ESP_LOGI(TAG, "flash kesz: %s", fw_path);
    } else {
        ui_draw_header("HIBA");
        display_oled_text_center(28, esp_err_to_name(err), 1);
        ESP_LOGE(TAG, "flash hiba: %s", esp_err_to_name(err));
    }
    display_oled_text_center(50, "Nyomj gombot", 1);
    display_oled_flush();

    /* Várunk egy gombnyomásra (bármilyen enkóder-eseményt elnyelünk). */
    enc_event_t ev;
    while (input_enc_get(&ev, portMAX_DELAY)) {
        if (ev == BTN_SHORT || ev == BTN_LONG) break;
    }
}

/* Cél-detektálás (SWD) a ui_task kontextusából, szinkron. Egy
   "Detektalas..." képernyőt mutat, majd a prog_session_detect() eredménye
   alapján kirajzolja a cél nevét/DEV_ID-jét/üzenetét (vagy "Nincs cel"/hiba),
   és egy gombnyomásig blokkol. A hívó utána visszaviszi a főmenübe. */
static void ui_start_detect(void)
{
    /* Ha épp flash/detekt fut, ne indítsunk párhuzamosan. */
    if (prog_session_busy()) {
        display_oled_clear();
        ui_draw_header("Cel info");
        display_oled_text_center(28, "Foglalt", 1);
        display_oled_flush();
        enc_event_t evb;
        while (input_enc_get(&evb, portMAX_DELAY)) {
            if (evb == BTN_SHORT || evb == BTN_LONG) break;
        }
        return;
    }

    /* "Detektalas..." képernyő a (potenciálisan lassú) SWD bring-up alatt. */
    display_oled_clear();
    ui_draw_header("Cel info");
    display_oled_text_center(28, "Detektalas...", 1);
    display_oled_flush();

    prog_status_t st;
    memset(&st, 0, sizeof(st));
    esp_err_t err = prog_session_detect(&st);

    display_oled_clear();
    if (err == ESP_OK && st.dev_id != 0) {
        /* Siker: cél neve + DEV_ID hexben + (ha van) üzenet/méret. */
        ui_draw_header("Cel info");
        display_oled_text(2, 18, st.target_name[0] ? st.target_name : "?", 1);

        char dbuf[20];
        snprintf(dbuf, sizeof(dbuf), "DEV: 0x%03X", st.dev_id);
        display_oled_text(2, 30, dbuf, 1);

        if (st.message[0]) {
            display_oled_text(2, 42, st.message, 1);
        }
        ESP_LOGI(TAG, "detekt OK: %s DEV=0x%03X %s",
                 st.target_name, st.dev_id, st.message);
    } else {
        /* Nincs cél vagy hiba: üzenet, vagy az esp_err név. */
        ui_draw_header("Cel info");
        display_oled_text_center(24, "Nincs cel", 1);
        const char *m = st.message[0] ? st.message : esp_err_to_name(err);
        display_oled_text_center(40, m, 1);
        ESP_LOGW(TAG, "detekt sikertelen: err=%s msg=%s",
                 esp_err_to_name(err), st.message);
    }
    display_oled_text_center(54, "Nyomj gombot", 1);
    display_oled_flush();

    /* Várunk egy gombnyomásra (bármilyen enkóder-eseményt elnyelünk). */
    enc_event_t ev;
    while (input_enc_get(&ev, portMAX_DELAY)) {
        if (ev == BTN_SHORT || ev == BTN_LONG) break;
    }
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
            } else if (s_sel == 1) {     /* Cel info (SWD) — szinkron detekt */
                ui_start_detect();
                /* Detekt után maradunk a főmenüben (újrarajzol a ui_task). */
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
        if (ev == BTN_SHORT) {
            /* Megerősítve: szinkron flash indítása a ui_task kontextusából.
               A hívás a teljes folyamat alatt blokkol (progress a cb-ben). */
            ui_start_flash();
            s_screen = SCR_FWLIST;   /* végén vissza a fájllistához */
        } else if (ev == BTN_LONG) {
            /* Mégse: vissza a fájllistához flash nélkül. */
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
