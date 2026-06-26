/* Cortex-M debug réteg: halt/resume/reset, connect-under-reset, core reg R/W.
 *
 * Az adiv5.h memória R/W primitíveire és a swd_phy.h nRST vezérlésére épül;
 * családfüggetlen Cortex-M debug mag. (terv 7. szekció)
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
       connect-under-reset feladata. Itt csak nyugalmi nRST szintet
       biztosítunk (reset elengedve). */
    swd_phy_nrst(false);
    return ESP_OK;
}

/* DHCSR S_HALT bit pollozása esp_timer alapú timeouttal. */
esp_err_t cortexm_wait_halt(uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    uint32_t dhcsr = 0;

    do {
        esp_err_t err = adiv5_read32(DHCSR, &dhcsr);
        if (err != ESP_OK) {
            return err;
        }
        if (dhcsr & DHCSR_S_HALT) {
            return ESP_OK;
        }
        /* Apró várakozás, hogy ne pörgessük teljes terhelésen az SWD buszt. */
        vTaskDelay(pdMS_TO_TICKS(1));
    } while (esp_timer_get_time() < deadline);

    /* Utolsó esély: lehet, hogy épp az utolsó iteráció után állt meg. */
    if (adiv5_read32(DHCSR, &dhcsr) == ESP_OK && (dhcsr & DHCSR_S_HALT)) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "wait_halt timeout (%u ms), DHCSR=0x%08x",
             (unsigned)timeout_ms, (unsigned)dhcsr);
    return ESP_ERR_TIMEOUT;
}

esp_err_t cortexm_halt(void)
{
    /* Debug engedélyezés + halt kérés, majd várás a tényleges megállásra. */
    esp_err_t err = adiv5_write32(DHCSR, DHCSR_HALT_CMD);
    if (err != ESP_OK) {
        return err;
    }
    return cortexm_wait_halt(HALT_POLL_TIMEOUT_MS);
}

esp_err_t cortexm_resume(void)
{
    /* Halt bit törlése, debug megtartva -> a mag fut tovább. */
    return adiv5_write32(DHCSR, DHCSR_RESUME_CMD);
}

esp_err_t cortexm_sysreset(void)
{
    /* SYSRESETREQ: rendszer reset kérés. A SWD/debug kapcsolat megmarad. */
    return adiv5_write32(AIRCR, AIRCR_SYSRESET);
}

esp_err_t cortexm_connect_under_reset(uint32_t *idcode_out)
{
    esp_err_t err;

    /* 1) nRST assert (reset aktív, alacsony szint) — a cél nem fut a bring-up alatt. */
    swd_phy_nrst(true);
    vTaskDelay(pdMS_TO_TICKS(10));   /* reset minimum hossz, beállás */

    /* 2) SWD bring-up reset alatt: line reset + JTAG->SWD switch + DPIDR + power-up. */
    err = adiv5_connect(idcode_out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adiv5_connect hiba: %s", esp_err_to_name(err));
        swd_phy_nrst(false);         /* ne hagyjuk resetben a célt */
        return err;
    }

    /* 3) Debug + halt engedélyezés még reset alatt (DHCSR <- 0xA05F0003). */
    err = adiv5_write32(DHCSR, DHCSR_HALT_CMD);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DHCSR halt-enable hiba: %s", esp_err_to_name(err));
        swd_phy_nrst(false);
        return err;
    }

    /* 4) Reset vector catch: a mag a reset elengedése után a reset-vektoron áll meg. */
    err = adiv5_write32(DEMCR, DEMCR_VC_CORERESET);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DEMCR VC_CORERESET hiba: %s", esp_err_to_name(err));
        swd_phy_nrst(false);
        return err;
    }

    /* 5) nRST release (reset elengedve) — a mag reset-et hajt végre, majd halt-ol. */
    swd_phy_nrst(false);
    vTaskDelay(pdMS_TO_TICKS(2));    /* a reset tényleges felfutása / kilépés */

    /* 6) Várás a halt állapotra (a reset-vektoron áll meg a VC_CORERESET miatt). */
    err = cortexm_wait_halt(HALT_POLL_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect-under-reset: nem állt meg halt-on");
        return err;
    }

    ESP_LOGI(TAG, "connect-under-reset OK, cél a reset-vektoron halt-ol");
    return ESP_OK;
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
    return adiv5_read32(DCRDR, val);
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

    /* Várás a transzfer befejezésére. */
    return wait_regrdy();
}
