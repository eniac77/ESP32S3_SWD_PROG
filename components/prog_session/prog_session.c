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

static const char *TAG = "prog_session";

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

    for (size_t i = 0; i < n; i++) {
        uint32_t val = 0;
        if (adiv5_read32(addrs[i], &val) != ESP_OK) continue;

        uint16_t dev_id = (uint16_t)(val & 0x0FFF);
        if (dev_id == 0) continue;

        const target_info_t *info = target_db_lookup(dev_id);
        if (info) {
            st->dev_id = dev_id;
            strncpy(st->target_name, info->name, sizeof(st->target_name) - 1);
            st->target_name[sizeof(st->target_name) - 1] = '\0';
            return info;
        }
        /* talált DEV_ID-t, de nincs a táblában — jegyezzük fel */
        st->dev_id = dev_id;
        strncpy(st->target_name, "?", sizeof(st->target_name) - 1);
        st->target_name[sizeof(st->target_name) - 1] = '\0';
    }
    return NULL;
}

/* Flash méret kiolvasása a családspecifikus F-size regiszterből (bájtban). */
static uint32_t read_flash_size(const target_info_t *info)
{
    uint32_t v = 0;
    if (adiv5_read32(info->flash_size_addr, &v) != ESP_OK) return 0;
    uint32_t kb = v & 0xFFFF;
    return kb * 1024u;
}

/* RDP (Readout Protection) szint kiolvasása a leíró rdp_kind/rdp_addr alapján.
   Visszaad: 0/1/2 az ismert szint, vagy -1 ha nem ismert/nem olvasható
   (RDP_REG_NONE, NULL info, vagy SWD olvasási hiba). */
static int read_rdp_level(const target_info_t *info)
{
    if (!info || info->rdp_kind == RDP_REG_NONE) {
        return -1;
    }
    uint32_t reg = 0;
    if (adiv5_read32(info->rdp_addr, &reg) != ESP_OK) {
        return -1;
    }
    return target_db_rdp_level(info, reg);
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

    /* 2. CONNECT fázis: fájl beolvasása LittleFS-ből. */
    emit(cb, ctx, &st, PROG_CONNECT, 0, "fajl olvasas");
    err = storage_lfs_read_all(fw_path, &data, &len);
    if (err != ESP_OK || !data || len == 0) {
        emit(cb, ctx, &st, PROG_FAILED, 0, "fajl olvasas hiba");
        err = (err == ESP_OK) ? ESP_FAIL : err;
        goto out;
    }

    /* 3. Connect-under-reset. */
    emit(cb, ctx, &st, PROG_CONNECT, 0, "connect");
    uint32_t dp_idcode = 0;
    err = cortexm_connect_under_reset(&dp_idcode);
    if (err != ESP_OK) {
        emit(cb, ctx, &st, PROG_FAILED, 0, "nincs cel / connect");
        goto out;
    }

    /* 4. Cél detektálás (DEV_ID). */
    const target_info_t *info = detect_target(&st);
    if (!info) {
        /* dev_id-t a status már tartalmazza (ha volt), de nincs leíró.
           Hibakód-taxonómia: konkrét DEV_ID-vel, ha van. */
        char msg[64];
        if (st.dev_id != 0) {
            snprintf(msg, sizeof(msg), "ismeretlen DEV_ID 0x%03X", (unsigned)st.dev_id);
        } else {
            snprintf(msg, sizeof(msg), "ismeretlen cel (nincs DEV_ID)");
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
            emit(cb, ctx, &st, PROG_FAILED, 0, "RDP2: vegleges zar");
            err = ESP_ERR_INVALID_STATE;
            goto out;
        }
        if (rdp == 1) {
            emit(cb, ctx, &st, PROG_FAILED, 0, "RDP1: mass-erase szukseges");
            err = ESP_ERR_INVALID_STATE;
            goto out;
        }
        /* rdp == 0 (vedtelen) vagy rdp < 0 (NONE / nem olvasható) -> folytatjuk. */
    }

    /* 5. Flash méret. */
    uint32_t flash_size = read_flash_size(info);

    /* 6. FLM kiválasztás — üres flm_blobs táblánál NULL a normális. */
    const flm_algo_t *algo = target_db_select_flm(info, flash_size);
    if (!algo) {
        emit(cb, ctx, &st, PROG_FAILED, 0, "nincs FLM");
        err = ESP_ERR_NOT_SUPPORTED;
        goto out;
    }

    /* 7. Bázis cím: explicit, különben az FLM leíró dev_addr-ja. */
    uint32_t base = base_addr ? base_addr : algo->dev_addr;

    /* 8. FLM betöltés + program (erase/program/verify) progress-híddal. */
    err = flm_runner_load(algo);
    if (err != ESP_OK) {
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
        emit(cb, ctx, &st, PROG_FAILED, st.percent, fmsg);
        goto out;
    }

    /* 9. Siker: reset & run, majd DONE 100%. */
    cortexm_sysreset();
    cortexm_resume();
    emit(cb, ctx, &st, PROG_DONE, 100, "kesz");
    err = ESP_OK;

out:
    /* 10. data felszabadítása minden ágon. */
    if (data) free(data);
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

    /* Connect-under-reset. */
    uint32_t dp_idcode = 0;
    err = cortexm_connect_under_reset(&dp_idcode);
    if (err != ESP_OK) {
        st.phase = PROG_FAILED;
        strncpy(st.message, "nincs cel / connect", sizeof(st.message) - 1);
        goto finish;
    }

    /* Detektálás + flash méret. */
    const target_info_t *info = detect_target(&st);
    if (!info) {
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

    snprintf(st.message, sizeof(st.message), "%s, %u KB, %s",
             info->name, (unsigned)(flash_size / 1024u), rdp_str);
    st.phase   = PROG_DONE;
    st.percent = 100;

finish:
    /* Nyugalmi állapot: reset elengedve, cél fut. */
    cortexm_sysreset();
    cortexm_resume();

    st.message[sizeof(st.message) - 1] = '\0';
    if (out) *out = st;
    xSemaphoreGive(s_lock);
    return err;
}
