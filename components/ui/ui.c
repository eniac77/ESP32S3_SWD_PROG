/*
 * ui.c — Helyi UI az ILI9488 480x320 + GT911 touch + enkóder fölött, LVGL v9-re.
 *
 * A korábbi SSD1306/OLED-implementáció (kézi framebuffer-rajz, input_enc queue
 * közvetlen olvasása) LEVÁLTVA. Mostantól:
 *   - a kijelzőt/touch/enkódert a `display_lcd` komponens hozza fel (LVGL-port
 *     + render-taszk + indevek). A ui.c CSAK LVGL-widgeteket épít.
 *   - a bevitelt az LVGL kezeli (touch pointer-indev + enkóder encoder-indev);
 *     a ui.c NEM olvassa az input_enc queue-t (annak EGYETLEN fogyasztója a
 *     display_lcd enc_read_cb-je).
 *   - a "vissza" (BTN_LONG) a display_lcd_set_back_cb()-en jön (port-taszk
 *     kontextus -> a cb-ben KÖZVETLENÜL hívunk lv_*()-t, lock NÉLKÜL).
 *
 * Képernyő-flow (a korábbi OLED-állapotgép funkcionálisan megőrizve):
 *   IDLE/status  --(klikk)-->  Főmenü
 *   Főmenü       --(0)-->  FW-lista --(klikk)--> FW-megerősítő --(OK)--> flash-progress
 *                --(1)-->  Cél-info (SWD detekt) eredmény
 *                --(2)-->  AVR detekt -> AVR-lista -> AVR-megerősítő -> AVR-flash
 *                --(3..5)-> placeholder ("hamarosan")
 *   Bárhol BTN_LONG (vagy a touch "Vissza" gomb) -> egy szinttel vissza.
 *
 * KRITIKUS — hosszú szinkron műveletek (flash/detect/AVR):
 *   ezek MÁSODPERCEKIG tartanak és NEM futhatnak az LVGL port-taszkon (befagyna
 *   a render) és nem is egy LVGL event-cb-ben. Ezért egy DEDIKÁLT worker-taszk
 *   (ui_worker) végzi: az LVGL gomb-esemény csak egy job-ot tesz a worker
 *   queue-jába, a worker hívja a prog_session / avr_isp hosszú hívásait, a progress-
 *   callback pedig `lvgl_port_lock` alatt frissíti az lv_bar-t/labeleket.
 *   A render a port-taszkon megy -> a flash alatt az UI reszponzív marad.
 *
 * UTF-8, magyar kommentek.  HW-n MÉG NEM IGAZOLT (D4) — a méret/elrendezés
 * néhány pontját "HW-n hangolandó" megjegyzés jelzi.
 */
#include "ui.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"          /* progress-throttle (LVGL-frissítés ritkítása) */

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "display_lcd.h"        /* display_lcd_group / display_lcd_set_back_cb */

#include "input_enc.h"          /* enc_event_t típusok (a flow-hoz; a queue-t a LCD olvassa) */
#include "storage_lfs.h"
#include "storage_src.h"
#include "prog_session.h"       /* SWD flash orchestráció */
#include "avr_isp.h"            /* AVR ISP programozó */
#include "target_state.h"       /* idle-állapot adatforrás */

#include "esp_log.h"

static const char *TAG = "ui";

/* ---------------------------------------------------------------------- */
/* Fájllista-puffer (statikus; max 32 név × 24 char) — a korábbi logikából */
/* ---------------------------------------------------------------------- */
#define FW_MAX_FILES   32
#define FW_NAME_LEN    24

static char    s_fw_names[FW_MAX_FILES][FW_NAME_LEN];
static int     s_fw_count;          /* érvényes nevek száma a pufferben     */
static char    s_sel_name[FW_NAME_LEN]; /* kiválasztott fw/avr fájl neve    */

/* ---------------------------------------------------------------------- */
/* Képernyő-azonosítók (a korábbi állapotgép logikája megőrizve)          */
/* ---------------------------------------------------------------------- */
typedef enum {
    SCR_IDLE,        /* állapot/status                          */
    SCR_MENU,        /* főmenü                                  */
    SCR_FWLIST,      /* SWD: firmware fájllista                 */
    SCR_FWSEL,       /* SWD: kiválasztott fw megerősítő         */
    SCR_PROGRESS,    /* SWD: flash progress                     */
    SCR_RESULT,      /* általános eredmény-/info-képernyő       */
    SCR_AVRLIST,     /* AVR: fájllista                          */
    SCR_AVRSEL,      /* AVR: kiválasztott fájl megerősítő       */
    SCR_AVRPROG,     /* AVR: flash progress                     */
    SCR_PLACEHOLDER, /* "(hamarosan)" almenük                   */
} ui_screen_t;

static ui_screen_t s_screen = SCR_IDLE;

/* Főmenü elemei — a SORREND/INDEXEK megőrizve (0=Program fw, 1=Cel info,
   2=AVR ISP, 3..5=placeholder), mert a click-handler index-alapú. */
static const char *const MENU_ITEMS[] = {
    "Program firmware",   /* 0 */
    "Cel info (SWD)",     /* 1 */
    "AVR ISP (ATtiny)",   /* 2 */
    "Cel konfig",         /* 3 — placeholder */
    "Elo adat",           /* 4 — placeholder */
    "Beallitasok",        /* 5 — placeholder */
};
#define MENU_COUNT ((int)(sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0])))

/* ---------------------------------------------------------------------- */
/* Aktív LVGL screen-objektumok + a progress-widgetek referenciái         */
/* ---------------------------------------------------------------------- */
/* A jelenleg megjelenített képernyő gyökér-objektuma. Váltáskor töröljük. */
static lv_obj_t *s_root = NULL;

/* Progress-képernyő widgetek (SWD és AVR közös) — a worker frissíti lock alatt. */
static lv_obj_t *s_prog_phase = NULL;   /* fázis-címke      */
static lv_obj_t *s_prog_name  = NULL;   /* cél/fájl neve    */
static lv_obj_t *s_prog_bar   = NULL;   /* lv_bar           */
static lv_obj_t *s_prog_pct   = NULL;   /* százalék-label   */

/* ---------------------------------------------------------------------- */
/* Worker-taszk — hosszú szinkron műveletek (flash/detect) a port-taszkon  */
/* kívül. Az LVGL event-cb-k csak job-ot tesznek a queue-ba.              */
/* ---------------------------------------------------------------------- */
typedef enum {
    JOB_FLASH_SWD,   /* SWD flash a kiválasztott fw-vel        */
    JOB_DETECT_SWD,  /* SWD cél-detektálás                     */
    JOB_DETECT_AVR,  /* AVR signature detektálás               */
    JOB_FLASH_AVR,   /* AVR flash a kiválasztott fájllal       */
} ui_job_t;

static QueueHandle_t s_job_q = NULL;

/* Előre-deklarációk (a build-screen függvények egymásra hivatkoznak). */
static void ui_show_idle(void);
static void ui_show_menu(void);
static void ui_show_fwlist(bool avr);
static void ui_show_progress(bool avr);
static void ui_show_result(const char *title, const char *line1,
                           const char *line2, bool ok);
static void post_job(ui_job_t job);
static void ui_confirm_ok_event(lv_event_t *e);

/* ====================================================================== */
/* Közös stílus-segédek                                                   */
/* ====================================================================== */

/* Fejléc-sáv egy képernyő tetején: cím-label sötét háttérrel.
   480px széles; a font a default Montserrat 14 (más font nincs befordítva). */
static lv_obj_t *make_header(lv_obj_t *parent, const char *title)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, LV_PCT(100), 40);             /* HW-n hangolandó magasság */
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 6, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1565C0), 0);  /* kék fejléc */
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(bar);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    return bar;
}

/* Új teljes-képernyős gyökér-objektum: az előzőt törli, friss konténert ad.
   A konténer kitölti a kijelzőt, sötét háttérrel, függőleges flex-elrendezéssel
   a fejléc alatt. A hívó a fejléc UTÁN adja a tartalmat. */
static lv_obj_t *new_screen_root(void)
{
    /* A korábbi képernyő widgetjeit (és így a hozzájuk tartozó group-tagságot)
       eldobjuk -> nincs lógó fókuszálható elem. */
    if (s_root) {
        lv_obj_del(s_root);
        s_root = NULL;
    }
    /* A progress-widget pointerek a törölt fán voltak -> nullázzuk. */
    s_prog_phase = s_prog_name = s_prog_bar = s_prog_pct = NULL;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_align(root, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    s_root = root;
    return root;
}

/* "Vissza" gomb a képernyő jobb felső sarkába (touch-hoz). Az enkóder a
   BTN_LONG-on jut vissza (display_lcd_set_back_cb). A click a megadott
   cb-t hívja (LVGL event-cb -> port-taszk -> NINCS extra lock). */
static void add_back_button(lv_obj_t *parent, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 90, 34);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -4, 3);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, "Vissza");
    lv_obj_center(l);
    /* A vissza-gombot is fókuszálhatóvá tesszük (enkóderrel is elérhető). */
    lv_group_add_obj(display_lcd_group(), btn);
}

/* ====================================================================== */
/* Fájllista-gyűjtés az aktív forrás /fw könyvtárából (a korábbi logika)  */
/* ====================================================================== */
static void fw_collect_cb(const char *name, size_t size, bool is_dir, void *ctx)
{
    (void)size; (void)ctx;
    if (is_dir) return;
    if (s_fw_count >= FW_MAX_FILES) return;
    strncpy(s_fw_names[s_fw_count], name, FW_NAME_LEN - 1);
    s_fw_names[s_fw_count][FW_NAME_LEN - 1] = '\0';
    s_fw_count++;
}

static void fw_list_load(void)
{
    s_fw_count = 0;
    char dir[48];
    snprintf(dir, sizeof(dir), "%s/fw", storage_src_base());
    esp_err_t err = storage_src_list(dir, fw_collect_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fw lista hiba: %s", esp_err_to_name(err));
    }
}

/* ====================================================================== */
/* Event-callbackek (mind az LVGL port-taszkon futnak -> NINCS extra lock)*/
/* ====================================================================== */

/* Globális "vissza" — a BTN_LONG (display_lcd_set_back_cb) ebbe fut. Egy
   szinttel feljebb lép a flow-ban. A worker-műveletek alatt (PROGRESS/AVRPROG)
   a vissza tiltott, amíg a művelet be nem fejeződik (a worker maga vált
   eredmény-képernyőre). */
static void ui_go_back(void)
{
    switch (s_screen) {
    case SCR_IDLE:
        /* már a tetején — nincs hova */
        break;
    case SCR_MENU:
        ui_show_idle();
        break;
    case SCR_FWLIST:
    case SCR_AVRLIST:
    case SCR_PLACEHOLDER:
    case SCR_RESULT:
        ui_show_menu();
        break;
    case SCR_FWSEL:
        ui_show_fwlist(false);
        break;
    case SCR_AVRSEL:
        ui_show_fwlist(true);
        break;
    case SCR_PROGRESS:
    case SCR_AVRPROG:
        /* Flash közben a vissza tiltott (a worker fejezi be). */
        break;
    }
}

/* A back-cb az LVGL indev read_cb-jéből hívódik (port-taszk) -> közvetlen
   lv_*() hívás engedélyezett, NE végy port-lock-ot (a szerződés szerint). */
static void back_cb_trampoline(void)
{
    ui_go_back();
}

/* Touch "Vissza" gomb event-cb (LVGL event -> port-taszk). */
static void back_btn_event(lv_event_t *e)
{
    (void)e;
    ui_go_back();
}

/* IDLE: bárhová kattintva (a "Tovabb a menube" gomb) -> főmenü. */
static void idle_enter_event(lv_event_t *e)
{
    (void)e;
    ui_show_menu();
}

/* Főmenü-elem kattintás: az elem indexét a user_data-ban tároljuk. */
static void menu_item_event(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    switch (idx) {
    case 0:  /* Program firmware */
        fw_list_load();
        ui_show_fwlist(false);
        break;
    case 1:  /* Cel info (SWD) — detekt a workeren */
        if (prog_session_busy()) {
            ui_show_result("Cel info", "Foglalt", "Mas muvelet fut", false);
        } else {
            ui_show_result("Cel info", "Detektalas...", "", true);
            s_screen = SCR_RESULT;       /* a worker felülírja az eredménnyel */
            post_job(JOB_DETECT_SWD);
        }
        break;
    case 2:  /* AVR ISP — detekt a workeren */
        if (prog_session_busy()) {
            ui_show_result("AVR ISP", "Foglalt", "Mas muvelet fut", false);
        } else {
            ui_show_result("AVR ISP", "Detektalas...", "", true);
            s_screen = SCR_RESULT;
            post_job(JOB_DETECT_AVR);
        }
        break;
    default: /* placeholder */
        new_screen_root();
        make_header(s_root, MENU_ITEMS[idx]);
        {
            lv_obj_t *l = lv_label_create(s_root);
            lv_label_set_text(l, "hamarosan");
            lv_obj_center(l);
            lv_obj_set_style_text_color(l, lv_color_white(), 0);
            add_back_button(s_root, back_btn_event);
        }
        s_screen = SCR_PLACEHOLDER;
        break;
    }
}

/* Fájllista-elem kattintás: a fájlnevet user_data-ból másoljuk, és a
   megerősítő képernyőre lépünk. Az 'avr' flag a user_data legalsó bitjén
   utazna — egyszerűbb: külön event-cb a két listához (lásd lent). */
static void fw_item_event(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    bool avr = (s_screen == SCR_AVRLIST);
    strncpy(s_sel_name, name, FW_NAME_LEN - 1);
    s_sel_name[FW_NAME_LEN - 1] = '\0';

    /* Megerősítő képernyő: lv_msgbox-szerű, de itt egy egyszerű konténer +
       két gomb (OK=flash / Megse). HW-n hangolandó méret. */
    new_screen_root();
    make_header(s_root, avr ? "AVR ISP" : "Program firmware");

    lv_obj_t *info = lv_label_create(s_root);
    lv_label_set_text_fmt(info, "Kivalasztva:\n%s", s_sel_name);
    lv_obj_set_style_text_color(info, lv_color_white(), 0);
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, 12, 56);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(info, LV_PCT(90));

    /* OK = flash gomb. A click-cb az aktuális képernyő (SCR_FWSEL/SCR_AVRSEL)
       alapján dönt SWD vs. AVR flash között -> egy közös cb elég. */
    lv_obj_t *ok = lv_button_create(s_root);
    lv_obj_set_size(ok, 180, 56);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_LEFT, 24, -24);
    lv_obj_add_event_cb(ok, ui_confirm_ok_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *okl = lv_label_create(ok);
    lv_label_set_text(okl, "OK = Flash");
    lv_obj_center(okl);
    lv_group_add_obj(display_lcd_group(), ok);

    /* Megse gomb. */
    lv_obj_t *cancel = lv_button_create(s_root);
    lv_obj_set_size(cancel, 180, 56);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_RIGHT, -24, -24);
    lv_obj_add_event_cb(cancel, back_btn_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Megse");
    lv_obj_center(cl);
    lv_group_add_obj(display_lcd_group(), cancel);

    s_screen = avr ? SCR_AVRSEL : SCR_FWSEL;
}

/* A megerősítő "OK = Flash" gomb event-cb-je: az aktuális képernyő alapján
   SWD vagy AVR flash-job indítása a workeren. */
static void ui_confirm_ok_event(lv_event_t *e)
{
    (void)e;
    if (prog_session_busy()) {
        ui_show_result("Flash", "Foglalt", "Mas muvelet fut", false);
        return;
    }
    bool avr = (s_screen == SCR_AVRSEL);
    ui_show_progress(avr);
    if (avr) {
        post_job(JOB_FLASH_AVR);
    } else {
        post_job(JOB_FLASH_SWD);
    }
}

/* ====================================================================== */
/* Képernyő-építők (mind a port-taszkon hívódnak: vagy event-cb-ből, vagy */
/* a worker lvgl_port_lock alól)                                          */
/* ====================================================================== */

/* 1. IDLE/status: cím + WiFi/Target/Serial labelek a target_state-ből. */
static void ui_show_idle(void)
{
    new_screen_root();
    make_header(s_root, "SWD PROG");

    target_state_t ts;
    target_state_get(&ts);

    char tbuf[64];

    lv_obj_t *cont = lv_obj_create(s_root);
    lv_obj_set_size(cont, LV_PCT(96), LV_PCT(70));
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l1 = lv_label_create(cont);
    /* A WiFi-állapotot a net_wifi nem teszi a target_state-be -> általános
       jelzés. HW-n hangolandó (ha kell, kösd be a net_wifi státuszt). */
    lv_label_set_text(l1, "WiFi:   ld. web UI");
    lv_obj_set_style_text_color(l1, lv_color_white(), 0);

    lv_obj_t *l2 = lv_label_create(cont);
    if (ts.target_present && ts.dev_id != 0) {
        snprintf(tbuf, sizeof(tbuf), "Target: %s (0x%03X)",
                 ts.target_name[0] ? ts.target_name : "?", ts.dev_id);
    } else {
        snprintf(tbuf, sizeof(tbuf), "Target: --");
    }
    lv_label_set_text(l2, tbuf);
    lv_obj_set_style_text_color(l2, lv_color_white(), 0);

    lv_obj_t *l3 = lv_label_create(cont);
    snprintf(tbuf, sizeof(tbuf), "Serial: %s", ts.serial_link ? "OK" : "--");
    lv_label_set_text(l3, tbuf);
    lv_obj_set_style_text_color(l3, lv_color_white(), 0);

    /* "Tovabb a menube" gomb (touch + enkóder). */
    lv_obj_t *btn = lv_button_create(s_root);
    lv_obj_set_size(btn, 220, 56);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_add_event_cb(btn, idle_enter_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Menu");
    lv_obj_center(bl);
    lv_group_add_obj(display_lcd_group(), btn);
    lv_group_focus_obj(btn);

    s_screen = SCR_IDLE;
}

/* 2. Főmenü: lv_list gombokkal, a MENU_ITEMS sorrendben. */
static void ui_show_menu(void)
{
    new_screen_root();
    make_header(s_root, "Fomenu");

    lv_obj_t *list = lv_list_create(s_root);
    lv_obj_set_size(list, LV_PCT(100), 280);            /* HW-n hangolandó */
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x101418), 0);

    for (int i = 0; i < MENU_COUNT; i++) {
        lv_obj_t *btn = lv_list_add_button(list, NULL, MENU_ITEMS[i]);
        lv_obj_add_event_cb(btn, menu_item_event, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_group_add_obj(display_lcd_group(), btn);
        if (i == 0) lv_group_focus_obj(btn);
    }

    s_screen = SCR_MENU;
}

/* 3. FW-lista (SWD) vagy AVR-lista (avr=true): lv_list a s_fw_names-ből.
   A fw_list_load() hívás a belépés előtt megtörténik. */
static void ui_show_fwlist(bool avr)
{
    new_screen_root();
    const char *title;
    bool usb = (storage_src_active() == STORAGE_SRC_USB);
    if (avr) title = usb ? "AVR fajl [USB]" : "AVR ISP fajl";
    else     title = usb ? "Firmware [USB]" : "Program firmware";
    make_header(s_root, title);
    add_back_button(s_root, back_btn_event);

    if (s_fw_count == 0) {
        lv_obj_t *l = lv_label_create(s_root);
        lv_label_set_text(l, avr ? "(nincs fajl)" : "(nincs fw fajl)");
        lv_obj_center(l);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
    } else {
        lv_obj_t *list = lv_list_create(s_root);
        lv_obj_set_size(list, LV_PCT(100), 270);
        lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 44);
        lv_obj_set_style_bg_color(list, lv_color_hex(0x101418), 0);

        for (int i = 0; i < s_fw_count; i++) {
            lv_obj_t *btn = lv_list_add_button(list, NULL, s_fw_names[i]);
            /* A user_data a NÉV stabil pointere (s_fw_names statikus). */
            lv_obj_add_event_cb(btn, fw_item_event, LV_EVENT_CLICKED,
                                (void *)s_fw_names[i]);
            lv_group_add_obj(display_lcd_group(), btn);
            if (i == 0) lv_group_focus_obj(btn);
        }
    }

    s_screen = avr ? SCR_AVRLIST : SCR_FWLIST;
}

/* 5. Progress-képernyő (SWD vagy AVR): fázis-label + cél/fájl + lv_bar + %. */
static void ui_show_progress(bool avr)
{
    new_screen_root();
    make_header(s_root, avr ? "AVR flash" : "SWD flash");

    s_prog_phase = lv_label_create(s_root);
    lv_label_set_text(s_prog_phase, "Indul...");
    lv_obj_set_style_text_color(s_prog_phase, lv_color_white(), 0);
    lv_obj_align(s_prog_phase, LV_ALIGN_TOP_LEFT, 16, 60);

    s_prog_name = lv_label_create(s_root);
    lv_label_set_text(s_prog_name, s_sel_name);
    lv_obj_set_style_text_color(s_prog_name, lv_color_white(), 0);
    lv_obj_align(s_prog_name, LV_ALIGN_TOP_LEFT, 16, 100);

    s_prog_bar = lv_bar_create(s_root);
    lv_obj_set_size(s_prog_bar, LV_PCT(90), 30);
    lv_obj_align(s_prog_bar, LV_ALIGN_CENTER, 0, 20);
    lv_bar_set_range(s_prog_bar, 0, 100);
    lv_bar_set_value(s_prog_bar, 0, LV_ANIM_OFF);

    s_prog_pct = lv_label_create(s_root);
    lv_label_set_text(s_prog_pct, "0%");
    lv_obj_set_style_text_color(s_prog_pct, lv_color_white(), 0);
    lv_obj_align(s_prog_pct, LV_ALIGN_CENTER, 0, 60);

    s_screen = avr ? SCR_AVRPROG : SCR_PROGRESS;
}

/* Általános eredmény-/info-képernyő: cím + 2 sor + OK gomb (vissza a menübe).
   Worker-kontextusból is hívható (a hívó tartja a port-lockot). */
static void ui_show_result(const char *title, const char *line1,
                           const char *line2, bool ok)
{
    new_screen_root();
    make_header(s_root, title);

    lv_obj_t *l1 = lv_label_create(s_root);
    lv_label_set_text(l1, line1 ? line1 : "");
    lv_obj_set_style_text_color(l1, ok ? lv_color_hex(0x4CAF50)
                                       : lv_color_hex(0xF44336), 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, 70);

    if (line2 && line2[0]) {
        lv_obj_t *l2 = lv_label_create(s_root);
        lv_label_set_text(l2, line2);
        lv_obj_set_style_text_color(l2, lv_color_white(), 0);
        lv_label_set_long_mode(l2, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l2, LV_PCT(90));
        lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, 120);
    }

    lv_obj_t *okb = lv_button_create(s_root);
    lv_obj_set_size(okb, 200, 56);
    lv_obj_align(okb, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_add_event_cb(okb, back_btn_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *okl = lv_label_create(okb);
    lv_label_set_text(okl, "OK");
    lv_obj_center(okl);
    lv_group_add_obj(display_lcd_group(), okb);
    lv_group_focus_obj(okb);

    s_screen = SCR_RESULT;
}

/* ====================================================================== */
/* Progress-frissítés thread-safe (a worker-taszkból, lock alatt)         */
/* ====================================================================== */

/* prog_phase_t -> rövid magyar fázis-címke. */
static const char *phase_label(prog_phase_t p)
{
    switch (p) {
    case PROG_CONNECT: return "Connect";
    case PROG_ERASE:   return "Torles";
    case PROG_PROGRAM: return "Iras";
    case PROG_VERIFY:  return "Ellenorzes";
    case PROG_DONE:    return "KESZ";
    case PROG_FAILED:  return "HIBA";
    default:           return "...";
    }
}

/* SWD progress callback — a worker-taszk kontextusából hívódik (NEM a
   port-taszk!). Ezért MINDEN lv_*() hívást lvgl_port_lock alá teszünk.
   THROTTLE (reference 5.): csak fázisváltáskor / terminál állapotban /
   >=200 ms-enként frissítünk, hogy a render ne lassítsa a flash-t. */
static void worker_swd_progress(const prog_status_t *st, void *ctx)
{
    (void)ctx;
    if (!st) return;

    bool terminal = (st->phase == PROG_DONE || st->phase == PROG_FAILED);
    static int64_t s_last_us = 0;
    static int s_last_phase = -1;
    int64_t now = esp_timer_get_time();
    if (!terminal && (int)st->phase == s_last_phase &&
        (now - s_last_us) < 200000) {
        return;
    }
    s_last_us = now;
    s_last_phase = (int)st->phase;

    int pct = st->percent;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    if (!lvgl_port_lock(0)) return;
    /* Védő: a worker még futhat, miközben a user már elnavigált (a widgetek
       törölve). A pointereket csak akkor használjuk, ha a progress-képernyő
       aktív. */
    if (s_prog_bar) {
        lv_label_set_text(s_prog_phase, phase_label(st->phase));
        if (st->target_name[0]) lv_label_set_text(s_prog_name, st->target_name);
        lv_bar_set_value(s_prog_bar, pct, LV_ANIM_OFF);
        char pbuf[8];
        snprintf(pbuf, sizeof(pbuf), "%d%%", pct);
        lv_label_set_text(s_prog_pct, pbuf);
    }
    lvgl_port_unlock();
}

/* AVR progress callback — ugyanaz a minta, a phase szöveget a hívó adja. */
static void worker_avr_progress(const char *phase, int percent, void *ctx)
{
    (void)ctx;
    static int64_t s_last_us = 0;
    static int s_last_pct = -1;
    bool terminal = (percent >= 100 || percent < 0);
    int64_t now = esp_timer_get_time();
    if (!terminal && percent == s_last_pct && (now - s_last_us) < 200000) {
        return;
    }
    s_last_us = now;
    s_last_pct = percent;

    int pct = percent;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    if (!lvgl_port_lock(0)) return;
    if (s_prog_bar) {
        lv_label_set_text(s_prog_phase, phase ? phase : "...");
        lv_bar_set_value(s_prog_bar, pct, LV_ANIM_OFF);
        char pbuf[8];
        snprintf(pbuf, sizeof(pbuf), "%d%%", pct);
        lv_label_set_text(s_prog_pct, pbuf);
    }
    lvgl_port_unlock();
}

/* ====================================================================== */
/* Worker-taszk — a job-queue-n blokkol, a hosszú szinkron hívásokat futtatja*/
/* ====================================================================== */

static void do_flash_swd(void)
{
    char fw_path[80];
    snprintf(fw_path, sizeof(fw_path), "%s/fw/%s", storage_src_base(), s_sel_name);
    ESP_LOGI(TAG, "SWD flash indul: %s", fw_path);

    esp_err_t err = prog_session_flash_file(fw_path, 0, worker_swd_progress, NULL);

    if (lvgl_port_lock(0)) {
        if (err == ESP_OK) {
            ui_show_result("KESZ", "Flash OK", s_sel_name, true);
            ESP_LOGI(TAG, "SWD flash kesz: %s", fw_path);
        } else {
            ui_show_result("HIBA", esp_err_to_name(err), s_sel_name, false);
            ESP_LOGE(TAG, "SWD flash hiba: %s", esp_err_to_name(err));
        }
        lvgl_port_unlock();
    }
}

static void do_detect_swd(void)
{
    prog_status_t st;
    memset(&st, 0, sizeof(st));
    esp_err_t err = prog_session_detect(&st);

    if (lvgl_port_lock(0)) {
        if (err == ESP_OK && st.dev_id != 0) {
            char l1[64];
            snprintf(l1, sizeof(l1), "%s (0x%03X)",
                     st.target_name[0] ? st.target_name : "?", st.dev_id);
            ui_show_result("Cel info", l1,
                           st.message[0] ? st.message : "", true);
            ESP_LOGI(TAG, "detekt OK: %s DEV=0x%03X %s",
                     st.target_name, st.dev_id, st.message);
        } else {
            ui_show_result("Cel info", "Nincs cel",
                           st.message[0] ? st.message : esp_err_to_name(err),
                           false);
            ESP_LOGW(TAG, "detekt sikertelen: err=%s msg=%s",
                     esp_err_to_name(err), st.message);
        }
        lvgl_port_unlock();
    }
}

static void do_detect_avr(void)
{
    avr_dev_t dev;
    memset(&dev, 0, sizeof(dev));
    esp_err_t err = avr_isp_detect(&dev);

    if (lvgl_port_lock(0)) {
        if (err == ESP_OK) {
            char l1[64], l2[64];
            snprintf(l1, sizeof(l1), "%s",
                     (dev.known && dev.name) ? dev.name : "ismeretlen");
            snprintf(l2, sizeof(l2), "SIG %02X %02X %02X  Flash %uB",
                     dev.sig[0], dev.sig[1], dev.sig[2],
                     (unsigned)dev.flash_size);
            /* Sikeres detekt -> tovább a fájllistához. A listát itt töltjük
               (a worker kontextusból a storage_src_list a saját lockját veszi,
               nem ütközik az LVGL-lel). Majd a lista-képernyőt rajzoljuk. */
            fw_list_load();
            ui_show_fwlist(true);
            /* A detekt-eredményt logoljuk; a fájllista a fő nézet. */
            ESP_LOGI(TAG, "AVR detekt OK: %s %s", l1, l2);
        } else {
            ui_show_result("AVR ISP", "Nincs cel", esp_err_to_name(err), false);
            ESP_LOGW(TAG, "AVR detekt sikertelen: %s", esp_err_to_name(err));
        }
        lvgl_port_unlock();
    }
}

static void do_flash_avr(void)
{
    char fw_path[80];
    snprintf(fw_path, sizeof(fw_path), "%s/fw/%s", storage_src_base(), s_sel_name);
    ESP_LOGI(TAG, "AVR flash indul: %s", fw_path);

    esp_err_t err = avr_isp_flash_file(fw_path, worker_avr_progress, NULL);

    if (lvgl_port_lock(0)) {
        if (err == ESP_OK) {
            ui_show_result("KESZ", "Flash OK", s_sel_name, true);
            ESP_LOGI(TAG, "AVR flash kesz: %s", fw_path);
        } else {
            ui_show_result("HIBA", esp_err_to_name(err), s_sel_name, false);
            ESP_LOGE(TAG, "AVR flash hiba: %s", esp_err_to_name(err));
        }
        lvgl_port_unlock();
    }
}

static void ui_worker(void *arg)
{
    (void)arg;
    ui_job_t job;
    for (;;) {
        if (xQueueReceive(s_job_q, &job, portMAX_DELAY) != pdTRUE) continue;
        switch (job) {
        case JOB_FLASH_SWD:  do_flash_swd();  break;
        case JOB_DETECT_SWD: do_detect_swd(); break;
        case JOB_DETECT_AVR: do_detect_avr(); break;
        case JOB_FLASH_AVR:  do_flash_avr();  break;
        }
    }
}

static void post_job(ui_job_t job)
{
    if (s_job_q) {
        xQueueSend(s_job_q, &job, 0);
    }
}

/* ====================================================================== */
/* Publikus API                                                           */
/* ====================================================================== */
esp_err_t ui_start(void)
{
    /* Job-queue + worker-taszk a hosszú szinkron műveletekhez. A worker-stack
       a SWD/FLM + storage hívásokhoz bőven méretezve (8 KB). */
    s_job_q = xQueueCreate(4, sizeof(ui_job_t));
    if (s_job_q == NULL) {
        ESP_LOGE(TAG, "job-queue letrehozas sikertelen");
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreate(ui_worker, "ui_worker", 8192, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "ui_worker letrehozas sikertelen");
        return ESP_ERR_NO_MEM;
    }

    /* A "vissza" (BTN_LONG / enkóder hosszú-nyomás) bekötése. A cb a port-
       taszkból (indev read_cb) hívódik -> közvetlen lv_*() (NINCS extra lock). */
    display_lcd_set_back_cb(back_cb_trampoline);

    /* A kezdő képernyő felépítése. A ui_start-ot a main hívja (nem a port-
       taszkon) -> a kezdeti widget-építést lock alá tesszük. */
    if (lvgl_port_lock(0)) {
        ui_show_idle();
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "lvgl_port_lock sikertelen (kezdo kepernyo)");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ui_start OK (LVGL v%d.%d, worker-taszk + job-queue)",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR);
    return ESP_OK;
}
