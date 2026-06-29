/* Cortex-M debug réteg: halt/resume/reset, csatlakozás, core reg R/W.
 *
 * Az adiv5.h memória R/W primitíveire épül; családfüggetlen Cortex-M debug mag.
 * (terv 7. szekció)
 *
 * FONTOS — nRST NÉLKÜLI működés: a cél áramkörökön a reset (nRST) láb tipikusan
 * NEM elérhető, ezért a csatlakozás tisztán SWD-n történik: a magot haltoljuk,
 * bekapcsoljuk a reset vektor-catch-et (DEMCR.VC_CORERESET), majd SZOFTVERES
 * rendszer-resetet adunk (AIRCR.SYSRESETREQ) — így a cél a reset-vektoron áll
 * meg, fizikai reset vonal nélkül. Az ESP nRST lába (swd_phy) megmarad más
 * célra, de a programozás nem függ tőle. (A C_DEBUGEN a debug-tápdomainben van,
 * a SYSRESETREQ nem törli — ezért marad a debug-kapcsolat a reseten át.)
 */
#include "cortexm_debug.h"
#include "adiv5.h"
#include "swd_phy.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cortexm";

/* --- Cortex-M debug regiszterek (System Control Space) --- */
#define DHCSR   0xE000EDF0u   /* Debug Halting Control and Status Register */
#define DCRSR   0xE000EDF4u   /* Debug Core Register Selector Register */
#define DCRDR   0xE000EDF8u   /* Debug Core Register Data Register */
#define DEMCR   0xE000EDFCu   /* Debug Exception and Monitor Control Register */
#define AIRCR   0xE000ED0Cu   /* Application Interrupt and Reset Control Register */

/* DHCSR írási kulcs + vezérlőbitek (felső 16 bit a DBGKEY). */
#define DHCSR_DBGKEY    0xA05F0000u
#define DHCSR_C_DEBUGEN (1u << 0)
#define DHCSR_C_HALT    (1u << 1)

/* DHCSR olvasási státuszbitek. */
#define DHCSR_S_REGRDY  (1u << 16)  /* core reg transfer kész */
#define DHCSR_S_HALT    (1u << 17)  /* a mag halt állapotban van */

/* Kész parancsszavak. */
#define DHCSR_HALT_CMD   (DHCSR_DBGKEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT) /* 0xA05F0003 */
#define DHCSR_RESUME_CMD (DHCSR_DBGKEY | DHCSR_C_DEBUGEN)               /* 0xA05F0001 */

/* DCRSR: REGSEL[6:0] + REGWnR (bit16: 1=write a magba, 0=read). */
#define DCRSR_REGWnR    (1u << 16)

/* DEMCR: reset vector catch -> a mag reset után azonnal halt-ol. */
#define DEMCR_VC_CORERESET (1u << 0)

/* AIRCR: VECTKEY + SYSRESETREQ (rendszer reset kérés). */
#define AIRCR_SYSRESET   0x05FA0004u

/* Pollozási alapértelmezések. */
#define HALT_POLL_TIMEOUT_MS  1000u   /* halt-ra várás default timeout */
#define REGRDY_TIMEOUT_MS     100u    /* core reg transfer timeout */

/* ------------------------------------------------------------------ */

esp_err_t cortexm_debug_init(void)
{
    /* Minimál: nincs perzisztens állapot, a tényleges bring-up a
       cortexm_connect_under_reset feladata. Az ESP nRST lábát nyugalmi
       (elengedett) szintre állítjuk — a cél-programozáshoz NEM használjuk,
       csak definiált állapotban tartjuk a fenntartott lábat. */
    swd_phy_nrst(false);
    ESP_LOGD(TAG, "cortexm_debug_init OK (nRST nyugalmi szinten, célhoz nem használt)");
    return ESP_OK;
}

/* DHCSR S_HALT bit pollozása esp_timer alapú timeouttal. */
esp_err_t cortexm_wait_halt(uint32_t timeout_ms)
{
    int64_t t0 = esp_timer_get_time();
    int64_t deadline = t0 + (int64_t)timeout_ms * 1000;
    uint32_t dhcsr = 0;

    do {
        esp_err_t err = adiv5_read32(DHCSR, &dhcsr);
        if (err != ESP_OK) {
            return err;
        }
        if (dhcsr & DHCSR_S_HALT) {
            ESP_LOGD(TAG, "halt elérve (DHCSR=0x%08x, %lld us alatt)",
                     (unsigned)dhcsr, (long long)(esp_timer_get_time() - t0));
            return ESP_OK;
        }
        /* Apró várakozás, hogy ne pörgessük teljes terhelésen az SWD buszt. */
        vTaskDelay(pdMS_TO_TICKS(1));
    } while (esp_timer_get_time() < deadline);

    /* Utolsó esély: lehet, hogy épp az utolsó iteráció után állt meg. */
    if (adiv5_read32(DHCSR, &dhcsr) == ESP_OK && (dhcsr & DHCSR_S_HALT)) {
        ESP_LOGD(TAG, "halt elérve (utolsó esély, DHCSR=0x%08x, %lld us)",
                 (unsigned)dhcsr, (long long)(esp_timer_get_time() - t0));
        return ESP_OK;
    }

    ESP_LOGW(TAG, "wait_halt timeout (%u ms), DHCSR=0x%08x",
             (unsigned)timeout_ms, (unsigned)dhcsr);
    return ESP_ERR_TIMEOUT;
}

esp_err_t cortexm_halt(void)
{
    /* Debug engedélyezés + halt kérés, majd várás a tényleges megállásra. */
    ESP_LOGD(TAG, "halt-kérés (DHCSR<-0x%08x)", (unsigned)DHCSR_HALT_CMD);
    esp_err_t err = adiv5_write32(DHCSR, DHCSR_HALT_CMD);
    if (err != ESP_OK) {
        return err;
    }
    return cortexm_wait_halt(HALT_POLL_TIMEOUT_MS);
}

esp_err_t cortexm_resume(void)
{
    /* Halt bit törlése, debug megtartva -> a mag fut tovább. */
    ESP_LOGD(TAG, "resume (DHCSR<-0x%08x)", (unsigned)DHCSR_RESUME_CMD);
    return adiv5_write32(DHCSR, DHCSR_RESUME_CMD);
}

esp_err_t cortexm_sysreset(void)
{
    /* SYSRESETREQ: rendszer reset kérés. A SWD/debug kapcsolat megmarad. */
    ESP_LOGD(TAG, "SYSRESETREQ küldése (AIRCR<-0x%08x)", (unsigned)AIRCR_SYSRESET);
    return adiv5_write32(AIRCR, AIRCR_SYSRESET);
}

esp_err_t cortexm_connect_under_reset(uint32_t *idcode_out)
{
#if CONFIG_CORTEXM_HW_NRST
    /* HARDVERES nRST ág (opcionális, Kconfig: CORTEXM_HW_NRST). Csak akkor
       használd, ha a cél-csatlakozón a reset (nRST) láb TÉNYLEG be van kötve.
       Klasszikus connect-under-reset: nRST assert alatt hozzuk fel a SWD-t és
       kapcsoljuk be a reset vektor-catch-et, majd elengedjük a reset-et — a mag
       a release után a reset-vektoron áll meg. A végeredmény ugyanaz a kontakt,
       mint a szoftveres ágban (halted-at-reset-vector). */
    esp_err_t err;

    ESP_LOGI(TAG, "connect-under-reset bring-up indul (HW-nRST ág)");

    /* 1) Reset aktiválása (nRST alacsony), majd rövid várakozás, hogy a cél
       biztosan reset alatt legyen, mire a SWD bring-up indul. */
    ESP_LOGD(TAG, "nRST assert (alacsony), 10 ms várakozás");
    swd_phy_nrst(true);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 2) SWD bring-up reset alatt: line reset + JTAG->SWD switch + DPIDR +
       debug power-up. A debug port külön táp-domainben van, így reset alatt is
       elérhető. */
    ESP_LOGD(TAG, "SWD bring-up reset alatt (adiv5_connect)");
    err = adiv5_connect(idcode_out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adiv5_connect hiba (HW-nRST): %s", esp_err_to_name(err));
        swd_phy_nrst(false);   /* hibaág: mindig engedjük el a reset-et */
        return err;
    }

    /* 3) Debug engedélyezés + halt kérés (DHCSR <- 0xA05F0003). */
    ESP_LOGD(TAG, "halt-enable (DHCSR<-0x%08x)", (unsigned)DHCSR_HALT_CMD);
    err = adiv5_write32(DHCSR, DHCSR_HALT_CMD);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DHCSR halt-enable hiba (HW-nRST): %s", esp_err_to_name(err));
        swd_phy_nrst(false);
        return err;
    }

    /* 4) Reset vector catch: a reset elengedése után a mag a reset-vektoron
       halt-ol (DEMCR.VC_CORERESET). */
    ESP_LOGD(TAG, "VC_CORERESET bekapcsolása (DEMCR<-0x%08x)", (unsigned)DEMCR_VC_CORERESET);
    err = adiv5_write32(DEMCR, DEMCR_VC_CORERESET);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DEMCR VC_CORERESET hiba (HW-nRST): %s", esp_err_to_name(err));
        swd_phy_nrst(false);
        return err;
    }

    /* 5) Reset elengedése (nRST magas). A mag újraindul, de a vektor-catch miatt
       azonnal megáll a reset-vektoron. */
    ESP_LOGD(TAG, "nRST elengedése (magas), 2 ms várakozás a felfutásra");
    swd_phy_nrst(false);
    vTaskDelay(pdMS_TO_TICKS(2));    /* a reset felfutása / mag indulása */

    /* 6) Várás a halt-ra (reset-vektoron). */
    ESP_LOGD(TAG, "várás halt-ra a reset-vektoron");
    err = cortexm_wait_halt(HALT_POLL_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "csatlakozas (HW-nRST): nem allt meg halt-on");
        return err;
    }

    /* 7) Vektor-catch kikapcsolása: innen a flash resume/halt ciklusai szabadon
       mennek, a végi reset&run pedig tisztán fut. */
    ESP_LOGD(TAG, "vektor-catch kikapcsolása (DEMCR<-0)");
    (void)adiv5_write32(DEMCR, 0u);

    ESP_LOGI(TAG, "csatlakozas (HW-nRST) OK, cel a reset-vektoron halt-ol");
    return ESP_OK;
#else
    /* nRST NÉLKÜLI csatlakozás (SYSRESETREQ + vektor-catch). A cél áramkörön a
       reset láb nem elérhető, ezért tisztán SWD-n hozunk létre "reset-vektoron
       halt-oló" állapotot. (Az ESP nRST lábát NEM használjuk a célhoz.)

       RETRY: egy alkalmi SWD-glitch deszinkronizálhat a bring-up közben, MIELŐTT
       a SYSRESETREQ-ig eljutnánk. Mivel egy TISZTA lefutás megfogja a magot a
       reset-vektoron (onnantól a mag áll -> stabil SWD), az EGÉSZ connect-
       szekvenciát újrapróbáljuk. A boot-önteszt bizonyítja, hogy tiszta connect
       lehetséges; néhány próbálkozás elkapja a tiszta ablakot. */
    const int CONNECT_ATTEMPTS = 6;
    esp_err_t err = ESP_FAIL;

    ESP_LOGI(TAG, "connect bring-up indul (nRST nélküli ág, SYSRESETREQ + vektor-catch)");

    for (int attempt = 1; attempt <= CONNECT_ATTEMPTS; attempt++) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "connect újrapróba #%d/%d (előző hiba: %s)",
                     attempt, CONNECT_ATTEMPTS, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(30));   /* hagyjuk a bus-t/célt rendeződni */
        }

        /* 1) SWD bring-up: line reset + JTAG->SWD switch + DPIDR + debug power-up.
           Ez a FUTÓ magon is működik (a debug port külön táp-domainben van). */
        ESP_LOGD(TAG, "SWD bring-up (adiv5_connect) a futó magon");
        err = adiv5_connect(idcode_out);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "adiv5_connect hiba: %s", esp_err_to_name(err));
            continue;
        }

        /* 2) A futó mag azonnali haltolása (DHCSR <- 0xA05F0003), hogy a reset
           előtt átvegyük az irányítást. A C_DEBUGEN a debug-domainben marad. */
        ESP_LOGD(TAG, "halt-enable a futó magon (DHCSR<-0x%08x)", (unsigned)DHCSR_HALT_CMD);
        err = adiv5_write32(DHCSR, DHCSR_HALT_CMD);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "DHCSR halt-enable hiba: %s", esp_err_to_name(err));
            continue;
        }
        ESP_LOGD(TAG, "várás halt-ra (bring-up, reset előtt)");
        err = cortexm_wait_halt(HALT_POLL_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "a mag nem allt meg halt-on (bring-up)");
            continue;
        }

        /* 3) Reset vector catch: a SYSRESETREQ után a mag a reset-vektoron halt-ol. */
        ESP_LOGD(TAG, "VC_CORERESET bekapcsolása (DEMCR<-0x%08x)", (unsigned)DEMCR_VC_CORERESET);
        err = adiv5_write32(DEMCR, DEMCR_VC_CORERESET);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "DEMCR VC_CORERESET hiba: %s", esp_err_to_name(err));
            continue;
        }

        /* 4) Szoftveres rendszer-reset (AIRCR SYSRESETREQ). A mag újraindul, majd a
           vektor-catch miatt azonnal halt-ol a reset-vektoron — fizikai nRST nélkül. */
        ESP_LOGD(TAG, "SYSRESETREQ küldve (AIRCR<-0x%08x), 2 ms várakozás", (unsigned)AIRCR_SYSRESET);
        err = adiv5_write32(AIRCR, AIRCR_SYSRESET);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "AIRCR SYSRESETREQ hiba: %s", esp_err_to_name(err));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(2));    /* a reset lefutása */

        /* 5) Várás a halt-ra (reset-vektoron). A reset alatt a DP/AP "stick"-elhet;
           ha közvetlenül nem kapunk halt-ot, egy újra-bring-up + ellenőrzés. */
        ESP_LOGD(TAG, "várás halt-ra a reset-vektoron");
        err = cortexm_wait_halt(HALT_POLL_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "reset utan nincs halt, ujra-bring-up...");
            if (adiv5_connect(NULL) != ESP_OK) {
                err = ESP_FAIL;
                continue;
            }
            ESP_LOGD(TAG, "újra-bring-up kész, ismételt várás halt-ra");
            err = cortexm_wait_halt(HALT_POLL_TIMEOUT_MS);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "reset után sem halt — újrapróba");
                continue;
            }
        }

        /* 6) Vektor-catch kikapcsolása: innen a flash resume/halt ciklusai szabadon
           mennek, a végi reset&run pedig tisztán fut (nem akad meg a vektoron). */
        ESP_LOGD(TAG, "vektor-catch kikapcsolása (DEMCR<-0)");
        (void)adiv5_write32(DEMCR, 0u);

        ESP_LOGI(TAG, "csatlakozas (nRST nelkul) OK (#%d. próbálkozás), cel a reset-vektoron halt-ol",
                 attempt);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "csatlakozas (nRST nelkul) sikertelen %d próbálkozás után: %s",
             CONNECT_ATTEMPTS, esp_err_to_name(err));
    return err;
#endif /* CONFIG_CORTEXM_HW_NRST */
}

/* S_REGRDY bit pollozása a core reg transzfer befejezésére. */
static esp_err_t wait_regrdy(void)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)REGRDY_TIMEOUT_MS * 1000;
    uint32_t dhcsr = 0;

    do {
        esp_err_t err = adiv5_read32(DHCSR, &dhcsr);
        if (err != ESP_OK) {
            return err;
        }
        if (dhcsr & DHCSR_S_REGRDY) {
            return ESP_OK;
        }
    } while (esp_timer_get_time() < deadline);

    ESP_LOGW(TAG, "S_REGRDY timeout, DHCSR=0x%08x", (unsigned)dhcsr);
    return ESP_ERR_TIMEOUT;
}

esp_err_t cortexm_reg_read(int regsel, uint32_t *val)
{
    if (val == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* DCRSR <- regsel (REGWnR=0: olvasás a magból). */
    esp_err_t err = adiv5_write32(DCRSR, (uint32_t)regsel & 0x7Fu);
    if (err != ESP_OK) {
        return err;
    }

    /* Várás a transzfer befejezésére. */
    err = wait_regrdy();
    if (err != ESP_OK) {
        return err;
    }

    /* Az érték a DCRDR-ben áll elő. */
    err = adiv5_read32(DCRDR, val);
    if (err == ESP_OK) {
        ESP_LOGV(TAG, "reg[%d] -> 0x%08lx", regsel, (unsigned long)*val);
    }
    return err;
}

esp_err_t cortexm_reg_write(int regsel, uint32_t val)
{
    /* Előbb az adat a DCRDR-be. */
    esp_err_t err = adiv5_write32(DCRDR, val);
    if (err != ESP_OK) {
        return err;
    }

    /* DCRSR <- regsel | REGWnR (write a magba). */
    err = adiv5_write32(DCRSR, ((uint32_t)regsel & 0x7Fu) | DCRSR_REGWnR);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGV(TAG, "reg[%d] <- 0x%08lx", regsel, (unsigned long)val);

    /* Várás a transzfer befejezésére. */
    return wait_regrdy();
}
