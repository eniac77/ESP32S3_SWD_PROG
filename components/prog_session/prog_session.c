#include "prog_session.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "cortexm_debug.h"
#include "adiv5.h"
#include "target_db.h"
#include "flm_runner.h"
#include "flm_blobs.h"
#include "storage_lfs.h"
#include "storage_src.h"
#include "net_wifi.h"
#include "swd_phy.h"

static const char *TAG = "prog_session";

/* SWD sebesség-profilok. Bring-up alatt lassú (megbízható), a tényleges
   adatfázis (FLM-load/program/verify) alatt feltolva. A WiFi-rádió leállítása
   óta a glitch-ráta elhanyagolható, így a magasabb SWCLK biztonságos. */
#define SWD_FREQ_BRINGUP   300000u    /* ~300 kHz: line reset + switch + connect */
#define SWD_FREQ_FAST           0u    /* 0 = nincs NOP-késleltetés -> a bit-bang NYERS
                                         maximuma (effektíve ~néhány MHz, a dedic_gpio
                                         hurok-overhead szabja). Power-cycle után 2 MHz
                                         10/10 ment (a korábbi 2 MHz-verify-hiba beragadt
                                         cél-állapot volt, nem a sebesség). A verify a
                                         biztonsági háló; ha a max túl gyors -> fix Hz-re. */

/* A per-tranzakció SWD-logok (VERBOSE) a flash idejének ~90%-át viszik az
   UART-on. A flash idejére INFO-ra vesszük a SWD-tag-eket (mérföldkövek
   maradnak), a végén visszaállítjuk VERBOSE-ra (a main.c baseline-ja). */
static void swd_logs_verbose(bool verbose)
{
    esp_log_level_t lv = verbose ? ESP_LOG_VERBOSE : ESP_LOG_INFO;
    esp_log_level_set("adiv5", lv);
    esp_log_level_set("cortexm", lv);
    esp_log_level_set("flm_runner", lv);
    esp_log_level_set("swd_phy", lv);
}

/* Belső lock: egyszerre csak egy flash/detektálás futhat. */
static SemaphoreHandle_t s_lock;

/* A progress-híd kontextusa: a flm_runner (phase,done,total)-ját
   prog_status_t-vá alakítja és a felhasználói cb-t hívja. */
typedef struct {
    prog_progress_cb user_cb;   /* felhasználói callback (lehet NULL) */
    void            *user_ctx;  /* felhasználói kontextus */
    prog_status_t    st;        /* aktuális állapot (dev_id/target_name megőrizve) */
} prog_bridge_t;

/* ---- segédek ---------------------------------------------------------- */

/* Állapot kitöltése + felhasználói cb hívása (ha van). */
static void emit(prog_progress_cb cb, void *ctx, prog_status_t *st,
                 prog_phase_t phase, int percent, const char *msg)
{
    st->phase   = phase;
    st->percent = percent;
    if (msg) {
        strncpy(st->message, msg, sizeof(st->message) - 1);
        st->message[sizeof(st->message) - 1] = '\0';
    }
    if (cb) cb(st, ctx);
}

/* Progress-híd: a flm_runner fázis-stringjét prog_phase_t-vá képezi,
   a százalékot done*100/total-ból számolja, és továbbítja a user cb-nek. */
static void flm_bridge_cb(const char *phase, uint32_t done, uint32_t total, void *ctx)
{
    prog_bridge_t *b = (prog_bridge_t *)ctx;
    if (!b->user_cb) return;

    prog_phase_t ph = PROG_PROGRAM;
    if (phase) {
        if      (strcmp(phase, "Erase")  == 0) ph = PROG_ERASE;
        else if (strcmp(phase, "Verify") == 0) ph = PROG_VERIFY;
        else                                   ph = PROG_PROGRAM;
    }

    int pct = (total > 0) ? (int)((uint64_t)done * 100 / total) : 0;
    if (pct > 100) pct = 100;

    b->st.phase   = ph;
    b->st.percent = pct;
    /* a message-t a fázisnévvel töltjük, dev_id/target_name megmarad */
    if (phase) {
        strncpy(b->st.message, phase, sizeof(b->st.message) - 1);
        b->st.message[sizeof(b->st.message) - 1] = '\0';
    }
    b->user_cb(&b->st, b->user_ctx);
}

/* Cél-detektálás: végigpróbálja az IDCODE-címeket, kitölti a status-t
   (dev_id, target_name) és visszaadja a target_info-t (vagy NULL).
   Feltételezi, hogy a cél már connect-under-reset állapotban van. */
static const target_info_t *detect_target(prog_status_t *st)
{
    size_t n = 0;
    const uint32_t *addrs = target_db_idcode_addrs(&n);
    ESP_LOGD(TAG, "detektálás: %u IDCODE-cím próbálása", (unsigned)n);

    for (size_t i = 0; i < n; i++) {
        uint32_t val = 0;
        if (adiv5_read32(addrs[i], &val) != ESP_OK) {
            ESP_LOGD(TAG, "IDCODE @ 0x%08lx -> olvasási hiba",
                     (unsigned long)addrs[i]);
            continue;
        }

        uint16_t dev_id = (uint16_t)(val & 0x0FFF);
        ESP_LOGD(TAG, "IDCODE @ 0x%08lx -> raw=0x%08lx dev_id=0x%03X",
                 (unsigned long)addrs[i], (unsigned long)val, (unsigned)dev_id);
        if (dev_id == 0) continue;

        const target_info_t *info = target_db_lookup(dev_id);
        if (info) {
            st->dev_id = dev_id;
            strncpy(st->target_name, info->name, sizeof(st->target_name) - 1);
            st->target_name[sizeof(st->target_name) - 1] = '\0';
            ESP_LOGI(TAG, "cél azonosítva: DEV_ID=0x%03X (%s) @ IDCODE 0x%08lx",
                     (unsigned)dev_id, info->name, (unsigned long)addrs[i]);
            return info;
        }
        /* talált DEV_ID-t, de nincs a táblában — jegyezzük fel */
        ESP_LOGW(TAG, "DEV_ID=0x%03X nincs a target_db-ben", (unsigned)dev_id);
        st->dev_id = dev_id;
        strncpy(st->target_name, "?", sizeof(st->target_name) - 1);
        st->target_name[sizeof(st->target_name) - 1] = '\0';
    }
    ESP_LOGW(TAG, "detektálás sikertelen: nincs ismert DEV_ID");
    return NULL;
}

/* Flash méret kiolvasása a családspecifikus F-size regiszterből (bájtban). */
static uint32_t read_flash_size(const target_info_t *info)
{
    uint32_t v = 0;
    if (adiv5_read32(info->flash_size_addr, &v) != ESP_OK) {
        ESP_LOGW(TAG, "flash méret olvasás hiba @ F-size reg 0x%08lx",
                 (unsigned long)info->flash_size_addr);
        return 0;
    }
    uint32_t kb = v & 0xFFFF;
    ESP_LOGD(TAG, "F-size reg 0x%08lx -> raw=0x%08lx -> %lu KB",
             (unsigned long)info->flash_size_addr, (unsigned long)v,
             (unsigned long)kb);
    return kb * 1024u;
}

/* RDP (Readout Protection) szint kiolvasása a leíró rdp_kind/rdp_addr alapján.
   Visszaad: 0/1/2 az ismert szint, vagy -1 ha nem ismert/nem olvasható
   (RDP_REG_NONE, NULL info, vagy SWD olvasási hiba). */
static int read_rdp_level(const target_info_t *info)
{
    if (!info || info->rdp_kind == RDP_REG_NONE) {
        ESP_LOGD(TAG, "RDP: nincs RDP regiszter a leíróban");
        return -1;
    }
    uint32_t reg = 0;
    if (adiv5_read32(info->rdp_addr, &reg) != ESP_OK) {
        ESP_LOGW(TAG, "RDP olvasás hiba @ 0x%08lx", (unsigned long)info->rdp_addr);
        return -1;
    }
    int lvl = target_db_rdp_level(info, reg);
    ESP_LOGD(TAG, "RDP reg 0x%08lx -> raw=0x%08lx -> szint=%d",
             (unsigned long)info->rdp_addr, (unsigned long)reg, lvl);
    return lvl;
}

/* ---- publikus API ----------------------------------------------------- */

esp_err_t prog_session_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            ESP_LOGE(TAG, "mutex letrehozas sikertelen");
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "init kesz");
    return ESP_OK;
}

bool prog_session_busy(void)
{
    if (!s_lock) return false;
    /* Ha most meg tudjuk fogni, nincs futó művelet -> azonnal el is engedjük. */
    if (xSemaphoreTake(s_lock, 0) == pdTRUE) {
        xSemaphoreGive(s_lock);
        return false;
    }
    return true;
}

esp_err_t prog_session_flash_file(const char *fw_path, uint32_t base_addr,
                                  prog_progress_cb cb, void *ctx)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;

    /* 1. Lock: ha már fut, INVALID_STATE. */
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    prog_status_t st = {0};
    st.target_name[0] = '?';
    st.target_name[1] = '\0';

    void     *data = NULL;
    size_t    len  = 0;
    esp_err_t err  = ESP_OK;

    ESP_LOGI(TAG, "flash indul: fájl='%s' base=0x%08lx",
             fw_path ? fw_path : "(nincs)", (unsigned long)base_addr);

    /* WiFi rádió leállítása a flash idejére: a rádió zaja glitch-eli a bit-bang
       SWD-t (HW-n megerősítve: re-sync 292 -> 2). Az enkóderes flasheléshez a
       WiFi nem kell. A végén (out:) visszakapcsoljuk. */
    (void)net_wifi_radio_pause(true);

    /* Csendes SWD-log a flash idejére (a per-tranzakció VERBOSE az idő ~90%-a). */
    swd_logs_verbose(false);

    /* 2. CONNECT fázis: fájl beolvasása az aktív forrásból (LFS vagy USB).
       A teljes fájlt PSRAM-pufferbe olvassuk MÉG a SWD/WiFi-pause előtt, így a
       stick a flash alatt már nem kell (kihúzás flash közben nem tör el). */
    emit(cb, ctx, &st, PROG_CONNECT, 0, "fajl olvasas");
    err = storage_src_read_all(fw_path, &data, &len);
    if (err != ESP_OK || !data || len == 0) {
        ESP_LOGE(TAG, "fájl olvasás hiba: '%s' err=%d len=%u",
                 fw_path ? fw_path : "(nincs)", err, (unsigned)len);
        emit(cb, ctx, &st, PROG_FAILED, 0, "fajl olvasas hiba");
        err = (err == ESP_OK) ? ESP_FAIL : err;
        goto out;
    }
    ESP_LOGI(TAG, "fajl betoltve %u B (%s)", (unsigned)len, fw_path);

    /* 3. Connect-under-reset. */
    emit(cb, ctx, &st, PROG_CONNECT, 0, "connect");
    uint32_t dp_idcode = 0;
    err = cortexm_connect_under_reset(&dp_idcode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect-under-reset hiba: err=%d (nincs cél?)", err);
        emit(cb, ctx, &st, PROG_FAILED, 0, "nincs cel / connect");
        goto out;
    }
    ESP_LOGI(TAG, "connect OK: DP IDCODE=0x%08lx", (unsigned long)dp_idcode);

    /* A bring-up kész (mag a reset-vektoron halt-ol) -> SWCLK feltolása az
       adatfázisra. A WiFi le van állítva, a glitch-ráta elhanyagolható. */
    swd_phy_set_freq_hz(SWD_FREQ_FAST);
    ESP_LOGI(TAG, "SWD órajel feltolva: %u Hz (adatfázis)", (unsigned)SWD_FREQ_FAST);

    /* 4. Cél detektálás (DEV_ID). */
    const target_info_t *info = detect_target(&st);
    if (!info) {
        /* dev_id-t a status már tartalmazza (ha volt), de nincs leíró.
           Hibakód-taxonómia: konkrét DEV_ID-vel, ha van. */
        char msg[64];
        if (st.dev_id != 0) {
            snprintf(msg, sizeof(msg), "ismeretlen DEV_ID 0x%03X", (unsigned)st.dev_id);
            ESP_LOGE(TAG, "ismeretlen DEV_ID 0x%03X (nincs leíró)", (unsigned)st.dev_id);
        } else {
            snprintf(msg, sizeof(msg), "ismeretlen cel (nincs DEV_ID)");
            ESP_LOGE(TAG, "ismeretlen cél: nem jött érvényes DEV_ID");
        }
        emit(cb, ctx, &st, PROG_FAILED, 0, msg);
        err = ESP_ERR_NOT_FOUND;
        goto out;
    }

    /* 4b. RDP (Readout Protection) ellenőrzés flash MEGKEZDÉSE ELŐTT.
       RDP1 -> csak mass-erase után írható (auto-unlock most nem cél);
       RDP2 -> végleges zár. Mindkét esetben tiszta hibakóddal kilépünk. */
    {
        int rdp = read_rdp_level(info);
        if (rdp >= 2) {
            ESP_LOGE(TAG, "RDP2: végleges zár -> programozás megszakítva");
            emit(cb, ctx, &st, PROG_FAILED, 0, "RDP2: vegleges zar");
            err = ESP_ERR_INVALID_STATE;
            goto out;
        }
        if (rdp == 1) {
            ESP_LOGE(TAG, "RDP1: mass-erase szükséges -> programozás megszakítva");
            emit(cb, ctx, &st, PROG_FAILED, 0, "RDP1: mass-erase szukseges");
            err = ESP_ERR_INVALID_STATE;
            goto out;
        }
        /* rdp == 0 (vedtelen) vagy rdp < 0 (NONE / nem olvasható) -> folytatjuk. */
        ESP_LOGI(TAG, "RDP szint=%d -> folytatás engedélyezve", rdp);
    }

    /* 5. Flash méret. */
    uint32_t flash_size = read_flash_size(info);
    ESP_LOGI(TAG, "DEV_ID=0x%03X (%s), flash=%lu KB",
             (unsigned)st.dev_id, info->name,
             (unsigned long)(flash_size / 1024u));

    /* 6. FLM kiválasztás — üres flm_blobs táblánál NULL a normális. */
    const flm_algo_t *algo = target_db_select_flm(info, flash_size);
    if (!algo) {
        ESP_LOGE(TAG, "nincs FLM a DEV_ID=0x%03X (%s) célhoz",
                 (unsigned)st.dev_id, info->name);
        emit(cb, ctx, &st, PROG_FAILED, 0, "nincs FLM");
        err = ESP_ERR_NOT_SUPPORTED;
        goto out;
    }
    ESP_LOGI(TAG, "kiválasztott FLM: '%s' (abi=%s)",
             algo->name ? algo->name : "(nincs)",
             (algo->abi == FLM_ABI_ST) ? "ST" : "CMSIS");

    /* 7. Bázis cím: explicit, különben az FLM leíró dev_addr-ja. */
    uint32_t base = base_addr ? base_addr : algo->dev_addr;
    ESP_LOGI(TAG, "flash bázis cím: 0x%08lx", (unsigned long)base);

    /* 8. FLM betöltés + program (erase/program/verify) progress-híddal. */
    ESP_LOGI(TAG, "FLM betöltése a cél RAM-jába");
    err = flm_runner_load(algo);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FLM betöltés/Init hiba: err=%d", err);
        emit(cb, ctx, &st, PROG_FAILED, 0, "Init hiba");
        goto out;
    }

    prog_bridge_t bridge = {
        .user_cb  = cb,
        .user_ctx = ctx,
        .st       = st,   /* dev_id + target_name átöröklése a hídba */
    };
    err = flm_runner_program(algo, base, (const uint8_t *)data, len,
                             flm_bridge_cb, &bridge);
    /* a híd által frissített dev_id/target_name visszamentése */
    st = bridge.st;
    if (err != ESP_OK) {
        /* Hibakód-taxonómia: a hiba abban a fázisban keletkezett, amit a
           progress-híd utoljára beállított (Erase/Program/Verify). */
        const char *fmsg = "Program hiba";
        if      (st.phase == PROG_ERASE)  fmsg = "Erase hiba";
        else if (st.phase == PROG_VERIFY) fmsg = "Verify hiba";
        else if (st.phase == PROG_PROGRAM)fmsg = "Program hiba";
        ESP_LOGE(TAG, "%s: err=%d (fázis=%d, %d%%)",
                 fmsg, err, (int)st.phase, st.percent);
        emit(cb, ctx, &st, PROG_FAILED, st.percent, fmsg);
        goto out;
    }

    /* 9. Siker: reset & run, majd DONE 100%. */
    ESP_LOGI(TAG, "programozás sikeres -> reset & run");
    cortexm_sysreset();
    cortexm_resume();
    emit(cb, ctx, &st, PROG_DONE, 100, "kesz");
    ESP_LOGI(TAG, "flash KÉSZ: DEV_ID=0x%03X (%s) 0x%08lx +%u B",
             (unsigned)st.dev_id, st.target_name,
             (unsigned long)base, (unsigned)len);
    err = ESP_OK;

out:
    /* 10. Visszaállítás minden ágon: SWCLK lassú, SWD-log verbose, WiFi vissza. */
    if (data) free(data);
    swd_phy_set_freq_hz(SWD_FREQ_BRINGUP);
    swd_logs_verbose(true);
    (void)net_wifi_radio_pause(false);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t prog_session_detect(prog_status_t *out)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    prog_status_t st = {0};
    st.target_name[0] = '?';
    st.target_name[1] = '\0';
    esp_err_t err = ESP_OK;

    ESP_LOGI(TAG, "detektálás indul");

    /* WiFi rádió leállítása a detektálás idejére is (azonos zaj-megfontolás). */
    (void)net_wifi_radio_pause(true);

    /* Connect-under-reset. */
    uint32_t dp_idcode = 0;
    err = cortexm_connect_under_reset(&dp_idcode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect-under-reset hiba: err=%d (nincs cél?)", err);
        st.phase = PROG_FAILED;
        strncpy(st.message, "nincs cel / connect", sizeof(st.message) - 1);
        goto finish;
    }
    ESP_LOGI(TAG, "connect OK: DP IDCODE=0x%08lx", (unsigned long)dp_idcode);

    /* Detektálás + flash méret. */
    const target_info_t *info = detect_target(&st);
    if (!info) {
        ESP_LOGE(TAG, "detektálás sikertelen: ismeretlen cél (dev_id=0x%03X)",
                 (unsigned)st.dev_id);
        st.phase = PROG_FAILED;
        strncpy(st.message, "ismeretlen cel", sizeof(st.message) - 1);
        err = ESP_ERR_NOT_FOUND;
        goto finish;
    }

    uint32_t flash_size = read_flash_size(info);

    /* RDP szint kiolvasása (ha a leíróhoz tartozik RDP regiszter). A detektálás
       erre NEM bukik el — csak informáljuk a felhasználót. */
    int rdp = read_rdp_level(info);
    const char *rdp_str = "RDP?";
    if      (rdp == 0) rdp_str = "RDP0";
    else if (rdp == 1) rdp_str = "RDP1";
    else if (rdp == 2) rdp_str = "RDP2";
    /* rdp < 0 (NONE / olvasási hiba) esetén "RDP?" marad. */
    if (rdp >= 1) {
        ESP_LOGW(TAG, "RDP védelem aktív: %s", rdp_str);
    }

    snprintf(st.message, sizeof(st.message), "%s, %u KB, %s",
             info->name, (unsigned)(flash_size / 1024u), rdp_str);
    st.phase   = PROG_DONE;
    st.percent = 100;
    ESP_LOGI(TAG, "detektálás kész: DEV_ID=0x%03X (%s), flash=%lu KB, %s",
             (unsigned)st.dev_id, info->name,
             (unsigned long)(flash_size / 1024u), rdp_str);

finish:
    /* Nyugalmi állapot: reset elengedve, cél fut. */
    cortexm_sysreset();
    cortexm_resume();

    (void)net_wifi_radio_pause(false);   /* WiFi rádió vissza */
    st.message[sizeof(st.message) - 1] = '\0';
    if (out) *out = st;
    xSemaphoreGive(s_lock);
    return err;
}
