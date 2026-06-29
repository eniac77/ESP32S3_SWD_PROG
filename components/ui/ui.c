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
#include "esp_timer.h"          /* progress-rajz throttle (OLED-flush ~25 ms) */

#include "display_oled.h"
#include "input_enc.h"
#include "storage_lfs.h"
#include "prog_session.h"   /* SWD flash orchestráció (prog_session_flash_file) */
#include "avr_isp.h"        /* AVR ISP programozó (ATtiny13 stb.) — párhuzamos flow */

#include "esp_log.h"

static const char *TAG = "ui";

/* ---------------------------------------------------------------------- */
/* Layout-konstansok (5x7 font: scale*6 px/karakter, scale*8 px magas)     */
/* NX80-stílus: nagy (scale 2) lista, keretes kijelölés.                   */
/* ---------------------------------------------------------------------- */
#define UI_HDR_H       11          /* fejléc-sáv magassága px-ben (scale 1)*/

/* --- NX80-stílusú lista-megjelenés (hangolható) --- */
#define UI_LIST_SCALE  2           /* listaelemek font-skálája (~14px magas)*/
#define UI_VISIBLE     3           /* egyszerre látható listaelemek száma  */
#define UI_ROW_H       16          /* sor-osztás px-ben (NX80: 16px)       */
#define UI_SEL_FRAME   1           /* kijelölés-stílus: 1 = keret, 0 = inverz */
#define UI_LIST_MAXCH  10          /* max karakter egy listasorban (scale 2)*/

/* A 3 látható listasor szöveg-y koordinátái (NX80 screen_menu mintára).
   A kijelölés-keret a sor-y - 1-től UI_ROW_H magasan rajzolódik. */
static const uint8_t UI_ROW_Y[UI_VISIBLE] = { 14, 31, 48 };

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
    SCR_AVRLIST,    /* AVR ISP: /lfs/fw fájllista       */
    SCR_AVRSEL,     /* AVR ISP: kiválasztott fájl megerősítő */
    SCR_PLACEHOLDER /* "(hamarosan)" almenük            */
} ui_screen_t;

static ui_screen_t s_screen   = SCR_IDLE;
static int         s_sel      = 0;   /* kiválasztott elem indexe          */
static int         s_scroll   = 0;   /* scroll-offset (első látható elem) */
static char        s_sel_name[FW_NAME_LEN]; /* kiválasztott fw neve       */
static const char *s_ph_title = "";  /* aktuális placeholder címe         */

/* Főmenü elemei (terv 15.2). A feliratok rövidítve, hogy scale 2-nél
   elférjenek (≤10 karakter); a SORREND/INDEXEK változatlanok, mert a
   ui_handle index-alapú elágazása ezekre épül (0=Program fw, 1=Cel info,
   2=AVR ISP, 3..5=placeholder). */
static const char *const MENU_ITEMS[] = {
    "Program fw",   /* 0 — "Program firmware" rövidítve */
    "Cel info",     /* 1 — "Cel info (SWD)"             */
    "AVR ISP",      /* 2 — "AVR ISP (ATtiny)"           */
    "Cel konfig",   /* 3 (10 char, fér)                 */
    "Elo adat",     /* 4                                */
    "Beallitas",    /* 5 — "Beallitasok" rövidítve      */
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

/* Szöveg-csonkolás egy fix karakterszámra (helyben, kis lokális pufferbe).
   A scale 2-es listához (max ~10 char/128px) kell, hogy ne lógjon ki a sor. */
static const char *ui_trunc(const char *src, char *buf, size_t bufsz, int maxch)
{
    if (maxch > (int)bufsz - 1) maxch = (int)bufsz - 1;
    int i = 0;
    for (; src[i] && i < maxch; i++) buf[i] = src[i];
    buf[i] = '\0';
    return buf;
}

/* Általános görgetett listarajzoló — NX80 screen_menu stílus:
 *   - fejléc (inverz sáv, scale 1) MARAD a tetején (kontextus),
 *   - legfeljebb UI_VISIBLE (3) elem scale 2-vel, x=2, sor-y UI_ROW_Y-ból,
 *   - a kiválasztott sor KERETTEL kiemelve (display_oled_rect, nem inverz).
 * A 'get_item' callback adja vissza az i. elem szövegét. */
typedef const char *(*ui_item_getter)(int idx);

static void ui_draw_list(const char *title, int count, int sel, int scroll,
                         ui_item_getter get_item, const char *empty_msg)
{
    display_oled_clear();
    ui_draw_header(title);

    if (count <= 0) {
        /* Üres lista: nagy (scale 2) középre igazított üzenet. */
        display_oled_text_center(28, empty_msg, UI_LIST_SCALE);
        display_oled_flush();
        return;
    }

    for (int row = 0; row < UI_VISIBLE; row++) {
        int idx = scroll + row;
        if (idx >= count) break;

        uint8_t y = UI_ROW_Y[row];
        char tbuf[UI_LIST_MAXCH + 1];
        const char *txt = ui_trunc(get_item(idx), tbuf, sizeof(tbuf), UI_LIST_MAXCH);

        /* A szöveg minden sornál sima (nem inverz) scale 2. */
        display_oled_text(2, y, txt, UI_LIST_SCALE);

        if (idx == sel) {
#if UI_SEL_FRAME
            /* Kijelölés: 1px keret a sor körül (NX80 screen_menu). */
            display_oled_rect(0, (uint8_t)(y - 1), OLED_W, UI_ROW_H);
#else
            /* (Opcionális) inverz kijelölés-stílus, ha UI_SEL_FRAME=0. */
            display_oled_fill_rect(0, (uint8_t)(y - 1), OLED_W, UI_ROW_H, true);
            for (int i = 0; txt[i]; i++)
                display_oled_char(2 + i * (UI_LIST_SCALE * 6), y, txt[i],
                                  UI_LIST_SCALE, false);
#endif
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

/* 1. Idle/status: cím + állapotsorok. Cím scale 2, az állapotsorok (≤10 char)
   szintén scale 2, nagyobb sor-osztással (y=18,36,52). */
static void draw_idle(void)
{
    display_oled_clear();
    display_oled_text_center(0, "SWD PROG", 2);
    display_oled_text(2, 18, "WiFi:   --", 2);
    display_oled_text(2, 36, "Target: --", 2);
    display_oled_text(2, 52, "Serial: --", 2);
    display_oled_flush();
}

/* 4. Placeholder almenük. Nagy (scale 2) felirat középen. */
static void draw_placeholder(void)
{
    display_oled_clear();
    ui_draw_header(s_ph_title);
    display_oled_text_center(28, "hamarosan", UI_LIST_SCALE);
    display_oled_flush();
}

/* 3b. Kiválasztott fw megerősítő képernyő. "Selected:"/"OK=flash" scale 2,
   a (hosszú lehet) fájlnév scale 1, hogy elférjen. */
static void draw_fwsel(void)
{
    display_oled_clear();
    ui_draw_header("Program fw");
    display_oled_text(2, 16, "Selected:", 2);
    display_oled_text(2, 34, s_sel_name, 1);
    display_oled_text(2, 48, "OK=flash", 2);   /* BTN_SHORT -> flash indul */
    display_oled_flush();
}

/* 3b/AVR. Kiválasztott AVR fájl megerősítő képernyő (.hex/.bin). */
static void draw_avrsel(void)
{
    display_oled_clear();
    ui_draw_header("AVR ISP");
    display_oled_text(2, 16, "Selected:", 2);
    display_oled_text(2, 34, s_sel_name, 1);
    display_oled_text(2, 48, "OK=flash", 2);   /* BTN_SHORT -> AVR flash indul */
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

    /* THROTTLE: a teljes OLED-flush ~25 ms (1 KB I2C @ 400 kHz). A progress-cb
       a program/verify alatt sok százszor hívódik (chunk/oldal), ezért MINDEN
       hívásra rajzolni dominálná a flash-időt (verify ~115 chunk × 25 ms ≈ 2,9 s).
       Csak fázisváltáskor, terminál állapotban (DONE/FAILED), vagy >=120 ms-enként
       rajzolunk -> ~7 fps progress, a flash-idő töredékéért. */
    bool terminal = (st->phase == PROG_DONE || st->phase == PROG_FAILED);
    static int64_t s_last_us = 0;
    static int s_last_phase = -1;
    int64_t now = esp_timer_get_time();
    /* 250 ms = ~4 fps progress: a feltöltés (program) ÉS az ellenőrzés (verify)
       alatt is ritkán rajzol, hogy az OLED-flush (~25 ms) ne lassítsa a flash-t. */
    if (!terminal && (int)st->phase == s_last_phase && (now - s_last_us) < 250000) {
        return;   /* túl gyakori -> kihagyjuk a rajzot */
    }
    s_last_us = now;
    s_last_phase = (int)st->phase;

    /* Százalék 0..100 közé szorítva (defenzív a bar-rajzhoz). */
    int pct = st->percent;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    display_oled_clear();
    ui_draw_header(phase_label(st->phase));

    /* Cél neve csonkolva (scale 2, max 10 char), illetve a százalék scale 2. */
    char nbuf[UI_LIST_MAXCH + 1];
    const char *nm = ui_trunc(st->target_name[0] ? st->target_name : "?",
                              nbuf, sizeof(nbuf), UI_LIST_MAXCH);
    display_oled_text(2, 16, nm, UI_LIST_SCALE);

    char pbuf[8];
    snprintf(pbuf, sizeof(pbuf), "%d%%", pct);
    display_oled_text(2, 34, pbuf, UI_LIST_SCALE);

    /* Progress-bar: 1px keret + belső kitöltés a százalék arányában. */
    const uint8_t bx = 2, by = 52, bw = OLED_W - 4, bh = 10;
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
        display_oled_text_center(26, "Flash OK", UI_LIST_SCALE);
        ESP_LOGI(TAG, "flash kesz: %s", fw_path);
    } else {
        ui_draw_header("HIBA");
        /* Az esp_err név hosszú lehet -> scale 1, hogy elférjen. */
        display_oled_text_center(28, esp_err_to_name(err), 1);
        ESP_LOGE(TAG, "flash hiba: %s", esp_err_to_name(err));
    }
    display_oled_text_center(52, "Nyomj gombot", 1);
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
        display_oled_text_center(28, "Foglalt", UI_LIST_SCALE);
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
    display_oled_text_center(28, "Detektalas", UI_LIST_SCALE);
    display_oled_flush();

    prog_status_t st;
    memset(&st, 0, sizeof(st));
    esp_err_t err = prog_session_detect(&st);

    display_oled_clear();
    if (err == ESP_OK && st.dev_id != 0) {
        /* Siker: cél neve (scale 2, csonkolva) + DEV_ID hexben (≤10 char, scale 2)
           + (ha van) üzenet scale 1-en (hosszú lehet). */
        ui_draw_header("Cel info");
        char nbuf[UI_LIST_MAXCH + 1];
        display_oled_text(2, 16,
                          ui_trunc(st.target_name[0] ? st.target_name : "?",
                                   nbuf, sizeof(nbuf), UI_LIST_MAXCH),
                          UI_LIST_SCALE);

        char dbuf[20];
        snprintf(dbuf, sizeof(dbuf), "DEV:0x%03X", st.dev_id);
        display_oled_text(2, 34, dbuf, UI_LIST_SCALE);

        /* (Az st.message hosszú lehet és ütközne a prompttal — kihagyjuk;
           a kulcs-info a név + DEV_ID. A teljes üzenet a logban marad.) */
        ESP_LOGI(TAG, "detekt OK: %s DEV=0x%03X %s",
                 st.target_name, st.dev_id, st.message);
    } else {
        /* Nincs cél vagy hiba: nagy "Nincs cel", alatta üzenet scale 1. */
        ui_draw_header("Cel info");
        display_oled_text_center(20, "Nincs cel", UI_LIST_SCALE);
        const char *m = st.message[0] ? st.message : esp_err_to_name(err);
        display_oled_text_center(42, m, 1);
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

/* ---------------------------------------------------------------------- */
/* AVR ISP flow — detekt + flash progress (mirror a SWD-ének, terv 15)    */
/* ---------------------------------------------------------------------- */

/* avr_isp progress callback: a flashelő (itt: ui_task) kontextusából hívódik
   minden fázis-/százalék-váltáskor. Egy teljes képernyőt rajzol: fázis-címke
   (fejléc, a hívótól kapott 'phase' szöveg), a fájlnév és egy százalékos
   progress-bar. A SWD ui_flash_cb mintájára (clear -> rajz -> flush). */
static void ui_avr_flash_cb(const char *phase, int percent, void *ctx)
{
    (void)ctx;

    /* Százalék 0..100 közé szorítva (defenzív a bar-rajzhoz). */
    int pct = percent;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    display_oled_clear();
    ui_draw_header(phase ? phase : "...");

    /* A flashelt fájl neve csonkolva (scale 2), illetve a százalék scale 2. */
    char nbuf[UI_LIST_MAXCH + 1];
    const char *nm = ui_trunc(s_sel_name[0] ? s_sel_name : "?",
                              nbuf, sizeof(nbuf), UI_LIST_MAXCH);
    display_oled_text(2, 16, nm, UI_LIST_SCALE);

    char pbuf[8];
    snprintf(pbuf, sizeof(pbuf), "%d%%", pct);
    display_oled_text(2, 34, pbuf, UI_LIST_SCALE);

    /* Progress-bar: 1px keret + belső kitöltés a százalék arányában. */
    const uint8_t bx = 2, by = 52, bw = OLED_W - 4, bh = 10;
    display_oled_rect(bx, by, bw, bh);
    int fill = ((bw - 2) * pct) / 100;
    if (fill > 0) {
        display_oled_fill_rect(bx + 1, by + 1, (uint8_t)fill, bh - 2, true);
    }

    display_oled_flush();
}

/* A kiválasztott AVR fájl szinkron flashelése a ui_task kontextusából. A SWD
   ui_start_flash mintájára: a hívás végéig blokkol (progress a cb-ben), végén
   eredmény-képernyő, majd egy gombnyomásig vár; a hívó visz vissza a listához. */
static void ui_avr_start_flash(void)
{
    char fw_path[64];
    snprintf(fw_path, sizeof(fw_path), STORAGE_LFS_BASE "/fw/%s", s_sel_name);

    ESP_LOGI(TAG, "AVR flash indul: %s", fw_path);
    esp_err_t err = avr_isp_flash_file(fw_path, ui_avr_flash_cb, NULL);

    /* Eredmény-képernyő: OK -> "KESZ", hiba -> esp_err név (rövid). */
    display_oled_clear();
    if (err == ESP_OK) {
        ui_draw_header("KESZ");
        display_oled_text_center(26, "Flash OK", UI_LIST_SCALE);
        ESP_LOGI(TAG, "AVR flash kesz: %s", fw_path);
    } else {
        ui_draw_header("HIBA");
        display_oled_text_center(28, esp_err_to_name(err), 1);
        ESP_LOGE(TAG, "AVR flash hiba: %s", esp_err_to_name(err));
    }
    display_oled_text_center(52, "Nyomj gombot", 1);
    display_oled_flush();

    /* Várunk egy gombnyomásra (bármilyen enkóder-eseményt elnyelünk). */
    enc_event_t ev;
    while (input_enc_get(&ev, portMAX_DELAY)) {
        if (ev == BTN_SHORT || ev == BTN_LONG) break;
    }
}

/* AVR cél-detektálás a ui_task kontextusából, szinkron (mirror a SWD
   ui_start_detect-jének). "Detektalas..." képernyőt mutat, majd az
   avr_isp_detect() eredménye alapján kirajzolja a signature-t, az eszköznevet
   és a flash méretet (vagy "Nincs cel"/hiba), és egy gombnyomásig blokkol.
   Sikeres detekt esetén true-t ad vissza (a hívó tovább a fájllistához). */
static bool ui_avr_start_detect(void)
{
    /* "Detektalas..." képernyő a (potenciálisan lassú) ISP bring-up alatt. */
    display_oled_clear();
    ui_draw_header("AVR ISP");
    display_oled_text_center(28, "Detektalas", UI_LIST_SCALE);
    display_oled_flush();

    avr_dev_t dev;
    memset(&dev, 0, sizeof(dev));
    esp_err_t err = avr_isp_detect(&dev);

    bool ok = (err == ESP_OK);

    display_oled_clear();
    ui_draw_header("AVR ISP");
    if (ok) {
        /* Eszköznév (ismert) scale 2, csonkolva — ez a kulcs-info. */
        char nbuf[UI_LIST_MAXCH + 1];
        display_oled_text(2, 14,
                          ui_trunc((dev.known && dev.name) ? dev.name : "ismeretlen",
                                   nbuf, sizeof(nbuf), UI_LIST_MAXCH),
                          UI_LIST_SCALE);

        /* Signature 3 bájt hexben (hosszú -> scale 1). */
        char sbuf[24];
        snprintf(sbuf, sizeof(sbuf), "SIG:%02X %02X %02X",
                 dev.sig[0], dev.sig[1], dev.sig[2]);
        display_oled_text(2, 34, sbuf, 1);

        /* Flash méret bájtban (scale 1). */
        char fbuf[24];
        snprintf(fbuf, sizeof(fbuf), "Flash:%uB", (unsigned)dev.flash_size);
        display_oled_text(2, 44, fbuf, 1);

        display_oled_text_center(54, "OK=tovabb", 1);
        ESP_LOGI(TAG, "AVR detekt OK: sig %02X %02X %02X %s flash=%u",
                 dev.sig[0], dev.sig[1], dev.sig[2],
                 (dev.known && dev.name) ? dev.name : "ismeretlen",
                 (unsigned)dev.flash_size);
    } else {
        display_oled_text_center(18, "Nincs cel", UI_LIST_SCALE);
        display_oled_text_center(42, esp_err_to_name(err), 1);
        display_oled_text_center(54, "Nyomj gombot", 1);
        ESP_LOGW(TAG, "AVR detekt sikertelen: err=%s", esp_err_to_name(err));
    }
    display_oled_flush();

    /* Várunk egy gombnyomásra. BTN_LONG = mégse (false), egyébként tovább. */
    enc_event_t ev;
    while (input_enc_get(&ev, portMAX_DELAY)) {
        if (ev == BTN_LONG) return false;
        if (ev == BTN_SHORT) break;
    }
    return ok;
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
    case SCR_AVRLIST:
        ui_draw_list("AVR ISP fajl", s_fw_count, s_sel, s_scroll,
                     fw_item, "(nincs fajl)");
        break;
    case SCR_AVRSEL:
        draw_avrsel();
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
            } else if (s_sel == 2) {     /* AVR ISP (ATtiny) — detekt -> fájllista */
                if (ui_avr_start_detect()) {
                    /* Sikeres detekt + OK: tovább a fájllistához. */
                    fw_list_load();
                    enter_screen(SCR_AVRLIST);
                }
                /* Sikertelen/mégse: maradunk a főmenüben (ui_task újrarajzol). */
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

    /* --- AVR ISP fájllista --- */
    case SCR_AVRLIST:
        switch (ev) {
        case ENC_CW:    list_move(+1, s_fw_count); break;
        case ENC_CCW:   list_move(-1, s_fw_count); break;
        case BTN_LONG:  enter_screen(SCR_MENU);    break;
        case BTN_SHORT:
            if (s_fw_count > 0) {
                strncpy(s_sel_name, s_fw_names[s_sel], FW_NAME_LEN - 1);
                s_sel_name[FW_NAME_LEN - 1] = '\0';
                s_screen = SCR_AVRSEL;
            }
            break;
        }
        break;

    /* --- AVR ISP kiválasztott fájl megerősítő --- */
    case SCR_AVRSEL:
        if (ev == BTN_SHORT) {
            /* Megerősítve: szinkron AVR flash a ui_task kontextusából. */
            ui_avr_start_flash();
            s_screen = SCR_AVRLIST;   /* végén vissza a fájllistához */
        } else if (ev == BTN_LONG) {
            /* Mégse: vissza a fájllistához flash nélkül. */
            s_screen = SCR_AVRLIST;
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
