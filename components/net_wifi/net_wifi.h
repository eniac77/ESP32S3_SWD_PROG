#pragma once
/* WiFi APSTA + mDNS. (terv 12.1)
 *
 * STA a meglévő hálózatra (creds NVS-ből), AP-fallback ha STA nem jön össze.
 * mDNS: swdprog.local.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_WIFI_DOWN,
    NET_WIFI_STA_CONNECTING,
    NET_WIFI_STA_GOT_IP,
    NET_WIFI_AP_FALLBACK,
} net_wifi_status_t;

/* NVS + netif + event loop + APSTA indítás + mDNS regisztrálás. */
esp_err_t net_wifi_init(void);

net_wifi_status_t net_wifi_get_status(void);

/* Aktuális IP string (STA vagy AP). true ha van érvényes IP. */
bool net_wifi_get_ip(char *buf, size_t len);

/* STA hitelesítő mentése NVS-be és újracsatlakozás. */
esp_err_t net_wifi_set_creds(const char *ssid, const char *pass);

/* WiFi rádió ideiglenes leállítása/visszakapcsolása. SWD-flash alatt a rádió
   zaja glitch-elheti a bit-bang SWD-t; a flash idejére érdemes leállítani.
   pause=true -> esp_wifi_stop(), pause=false -> esp_wifi_start(). Idempotens. */
esp_err_t net_wifi_radio_pause(bool pause);

#ifdef __cplusplus
}
#endif
