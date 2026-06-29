/* net_wifi.c — WiFi APSTA + mDNS (terv 12.1)
 *
 * Működés:
 *   - APSTA mód: a STA a meglévő hálózatra próbál csatlakozni (creds NVS-ből),
 *     az AP mindig elérhető fallbacknek (helyszíni / első konfiguráláshoz).
 *   - Ha nincs mentett SSID, vagy a STA többszöri próbálkozás után sem jön össze,
 *     az állapot AP_FALLBACK lesz (az AP-ra felcsatlakozva érhető el az eszköz).
 *   - mDNS: swdprog.local, _http._tcp szolgáltatás a 80-as porton.
 *
 * Minden szöveg UTF-8, bőséges ESP_LOG.
 */
#include "net_wifi.h"

#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"
#include "mdns.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "net_wifi";

/* ----- Konfigurációs állandók ----- */
#define NET_WIFI_AP_SSID        "SWDPROG-AP"     /* fallback AP SSID */
#define NET_WIFI_AP_PASS        "swdprog123"     /* fallback AP jelszó (min. 8 karakter -> WPA2) */
#define NET_WIFI_AP_CHANNEL     1
#define NET_WIFI_AP_MAX_CONN    4
#define NET_WIFI_AP_IP          "192.168.4.1"    /* alapértelmezett AP IP (esp_netif default) */

#define NET_WIFI_MDNS_HOST      "swdprog"        /* -> swdprog.local */
#define NET_WIFI_MDNS_INSTANCE  "ESP32-S3 SWD Programmer"

#define NET_WIFI_STA_MAX_RETRY  5                /* ennyi sikertelen próba után AP_FALLBACK */

/* NVS tárolás: namespace "wifi", kulcsok "ssid" / "pass" */
#define NET_WIFI_NVS_NS         "wifi"
#define NET_WIFI_NVS_KEY_SSID   "ssid"
#define NET_WIFI_NVS_KEY_PASS   "pass"

/* ----- Belső állapot ----- */
static net_wifi_status_t s_status = NET_WIFI_DOWN;
static char s_sta_ip[16] = {0};                  /* "xxx.xxx.xxx.xxx" */
static int  s_retry_cnt = 0;
static bool s_inited = false;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;

/* ============================================================
 *  NVS creds segédfüggvények
 * ============================================================ */

/* Mentett STA creds betöltése. true ha van érvényes (nem üres) SSID. */
static bool nvs_load_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NET_WIFI_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Nincs mentett wifi creds (nvs_open: %s)", esp_err_to_name(err));
        return false;
    }

    size_t sl = ssid_len;
    size_t pl = pass_len;
    if (ssid) ssid[0] = '\0';
    if (pass) pass[0] = '\0';

    err = nvs_get_str(h, NET_WIFI_NVS_KEY_SSID, ssid, &sl);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Nincs mentett SSID (%s)", esp_err_to_name(err));
        nvs_close(h);
        return false;
    }
    /* jelszó hiányozhat (nyílt háló) */
    esp_err_t perr = nvs_get_str(h, NET_WIFI_NVS_KEY_PASS, pass, &pl);
    if (perr != ESP_OK && pass) {
        pass[0] = '\0';
    }
    nvs_close(h);

    bool valid = (ssid && ssid[0] != '\0');
    if (valid) {
        ESP_LOGI(TAG, "Betöltött STA SSID: '%s'", ssid);
    }
    return valid;
}

/* STA creds mentése NVS-be. */
static esp_err_t nvs_save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NET_WIFI_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open (write) hiba: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(h, NET_WIFI_NVS_KEY_SSID, ssid ? ssid : "");
    if (err == ESP_OK) {
        err = nvs_set_str(h, NET_WIFI_NVS_KEY_PASS, pass ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Creds mentés hiba: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "STA creds elmentve NVS-be (ssid='%s')", ssid ? ssid : "");
    }
    return err;
}

/* ============================================================
 *  Esemény-kezelők (WiFi + IP)
 * ============================================================ */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            /* STA elindult -> connect kérése (csak ha van mentett SSID) */
            ESP_LOGI(TAG, "WIFI_EVENT_STA_START -> esp_wifi_connect()");
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "STA csatlakozva az AP-hoz, IP-re várunk...");
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            /* Néhányszor újrapróbálunk, utána AP-fallback állapot. */
            if (s_retry_cnt < NET_WIFI_STA_MAX_RETRY) {
                s_retry_cnt++;
                s_status = NET_WIFI_STA_CONNECTING;
                s_sta_ip[0] = '\0';
                ESP_LOGW(TAG, "STA disconnect, újrapróba %d/%d", s_retry_cnt, NET_WIFI_STA_MAX_RETRY);
                esp_wifi_connect();
            } else {
                s_status = NET_WIFI_AP_FALLBACK;
                s_sta_ip[0] = '\0';
                ESP_LOGW(TAG, "STA nem jött össze (%d próba) -> AP_FALLBACK (%s)",
                         s_retry_cnt, NET_WIFI_AP_IP);
            }
            break;
        }

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
            ESP_LOGI(TAG, "Kliens csatlakozott az AP-hoz: " MACSTR " (aid=%d)",
                     MAC2STR(e->mac), e->aid);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
            ESP_LOGI(TAG, "Kliens lecsatlakozott az AP-ról: " MACSTR " (aid=%d)",
                     MAC2STR(e->mac), e->aid);
            break;
        }

        default:
            break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
            s_retry_cnt = 0;
            s_status = NET_WIFI_STA_GOT_IP;
            snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
            ESP_LOGI(TAG, "IP_EVENT_STA_GOT_IP -> STA_GOT_IP, IP=%s", s_sta_ip);
        }
    }
}

/* ============================================================
 *  WiFi konfiguráció (APSTA)
 * ============================================================ */

static void wifi_apply_ap_config(void)
{
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, NET_WIFI_AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(NET_WIFI_AP_SSID);
    ap.ap.channel = NET_WIFI_AP_CHANNEL;
    ap.ap.max_connection = NET_WIFI_AP_MAX_CONN;

    if (strlen(NET_WIFI_AP_PASS) >= 8) {
        strlcpy((char *)ap.ap.password, NET_WIFI_AP_PASS, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_LOGI(TAG, "AP konfig: SSID='%s', auth=%s, IP=%s",
             NET_WIFI_AP_SSID,
             (ap.ap.authmode == WIFI_AUTH_OPEN) ? "OPEN" : "WPA2-PSK",
             NET_WIFI_AP_IP);
}

/* Beállítja a STA configot a megadott credsből és (újra)indítja a connectet.
 * Ha ssid==NULL, NVS-ből próbál tölteni. */
static void wifi_apply_sta_and_connect(const char *ssid_in, const char *pass_in)
{
    char ssid[33] = {0};
    char pass[65] = {0};

    if (ssid_in) {
        strlcpy(ssid, ssid_in, sizeof(ssid));
        if (pass_in) strlcpy(pass, pass_in, sizeof(pass));
    } else {
        if (!nvs_load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
            /* Nincs mentett SSID -> nem indítunk STA connectet, AP-fallback marad. */
            s_status = NET_WIFI_AP_FALLBACK;
            ESP_LOGW(TAG, "Nincs STA creds -> AP_FALLBACK (csatlakozz a '%s' AP-hoz)",
                     NET_WIFI_AP_SSID);
            return;
        }
    }

    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = (pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));

    s_retry_cnt = 0;
    s_status = NET_WIFI_STA_CONNECTING;
    s_sta_ip[0] = '\0';
    ESP_LOGI(TAG, "STA csatlakozás indul: SSID='%s' -> STA_CONNECTING", ssid);
    esp_wifi_connect();
}

/* ============================================================
 *  mDNS
 * ============================================================ */

static esp_err_t mdns_start(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init hiba: %s", esp_err_to_name(err));
        return err;
    }
    ESP_ERROR_CHECK(mdns_hostname_set(NET_WIFI_MDNS_HOST));
    ESP_ERROR_CHECK(mdns_instance_name_set(NET_WIFI_MDNS_INSTANCE));

    /* HTTP szolgáltatás meghirdetése a 80-as porton. */
    err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_service_add(_http) hiba: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "mDNS aktív: %s.local (_http._tcp:80)", NET_WIFI_MDNS_HOST);
    return ESP_OK;
}

/* ============================================================
 *  Publikus API
 * ============================================================ */

esp_err_t net_wifi_init(void)
{
    if (s_inited) {
        ESP_LOGW(TAG, "net_wifi_init már lefutott");
        return ESP_OK;
    }

    /* 1) NVS init (retry erase-re, ha a partíció sérült / verzió eltér). */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS erase szükséges (%s), törlés és újrainit", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* 2) netif + alapértelmezett event loop. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 3) STA + AP netif (APSTA). */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    /* 4) WiFi driver init. */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 5) Event handlerek regisztrálása. */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    /* 6) APSTA mód + AP konfig (AP mindig elérhető fallbacknek). */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_apply_ap_config();

    /* 7) STA config NVS-ből és (ha van SSID) connect. A STA configot
     *    még a start előtt beállítjuk; a connectet a STA_START esemény
     *    indítja, illetve itt is jelezzük az állapotot. */
    {
        char ssid[33] = {0};
        char pass[65] = {0};
        if (nvs_load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
            wifi_config_t sta = { 0 };
            strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
            strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
            sta.sta.threshold.authmode = (pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
            s_retry_cnt = 0;
            s_status = NET_WIFI_STA_CONNECTING;
            ESP_LOGI(TAG, "Mentett STA creds -> STA_CONNECTING (SSID='%s')", ssid);
        } else {
            s_status = NET_WIFI_AP_FALLBACK;
            ESP_LOGW(TAG, "Nincs STA creds -> AP_FALLBACK (csatlakozz a '%s' AP-hoz, IP %s)",
                     NET_WIFI_AP_SSID, NET_WIFI_AP_IP);
        }
    }

    /* 8) WiFi indítása. A STA_START handler hívja az esp_wifi_connect()-et,
     *    ha nincs SSID, a connect egyszerűen sikertelen lesz / nem csatlakozik. */
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 9) mDNS: swdprog.local + _http._tcp:80. */
    mdns_start();

    s_inited = true;
    ESP_LOGI(TAG, "net_wifi init kész (APSTA, AP='%s')", NET_WIFI_AP_SSID);
    return ESP_OK;
}

net_wifi_status_t net_wifi_get_status(void)
{
    return s_status;
}

bool net_wifi_get_ip(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return false;
    }

    /* Ha van érvényes STA IP, azt adjuk; különben az AP IP-t (192.168.4.1). */
    if (s_status == NET_WIFI_STA_GOT_IP && s_sta_ip[0] != '\0') {
        strlcpy(buf, s_sta_ip, len);
        return true;
    }

    if (s_inited) {
        /* AP mindig fut APSTA-ban -> AP IP elérhető. */
        strlcpy(buf, NET_WIFI_AP_IP, len);
        return true;
    }

    buf[0] = '\0';
    return false;
}

esp_err_t net_wifi_radio_pause(bool pause)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (pause) {
        ESP_LOGW(TAG, "WiFi rádió LEÁLLÍTÁSA (SWD-flash zajmentesítés)");
        esp_err_t err = esp_wifi_stop();
        return (err == ESP_ERR_WIFI_NOT_STARTED) ? ESP_OK : err;
    }
    ESP_LOGI(TAG, "WiFi rádió visszakapcsolása");
    esp_err_t err = esp_wifi_start();
    return (err == ESP_ERR_WIFI_NOT_STOPPED) ? ESP_OK : err;
}

esp_err_t net_wifi_set_creds(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') {
        ESP_LOGE(TAG, "net_wifi_set_creds: üres SSID");
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited) {
        ESP_LOGE(TAG, "net_wifi_set_creds: init még nem futott le");
        return ESP_ERR_INVALID_STATE;
    }

    /* 1) Mentés NVS-be. */
    esp_err_t err = nvs_save_creds(ssid, pass);
    if (err != ESP_OK) {
        return err;
    }

    /* 2) Újracsatlakozás STA-ként: előbb leválasztjuk a régit. */
    ESP_LOGI(TAG, "Új STA creds -> újracsatlakozás (SSID='%s')", ssid);
    esp_wifi_disconnect();
    wifi_apply_sta_and_connect(ssid, pass);
    return ESP_OK;
}
