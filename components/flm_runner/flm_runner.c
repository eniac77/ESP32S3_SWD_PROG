/* CMSIS FLM futtató implementáció. (terv 8. szekció)
 *
 * RAM-elrendezés a cél 0x20000000 SRAM-jában:
 *   [PrgCode][PrgData][BKPT-szó][stack ↓][buffer]
 *
 * A PrgCode/PrgData a kód mögé töltődik, a BKPT-szó a call_function
 * visszatérési pontja (LR ide mutat), a stack lefelé nő a BKPT fölül
 * fenntartott területen, a buffer pedig a stack teteje fölött kap helyet
 * (ide kerül a programozandó page tartalma).
 *
 * Csak a cortexm_debug + adiv5 rétegre épül.
 */
#include <string.h>
#include "esp_log.h"

#include "flm_runner.h"
#include "flm_blobs.h"
#include "cortexm_debug.h"
#include "adiv5.h"

static const char *TAG = "flm_runner";

/* --- RAM-elrendezés alapértékek --- */
#define RAM_BASE        0x20000000u  /* cél SRAM bázis (Cortex-M konvenció) */
#define ALIGN8(x)       (((x) + 7u) & ~7u)
#define STACK_SIZE      0x400u       /* 1 KB stack a FLM rutinoknak */
#define BUFFER_SIZE     0x1000u      /* page-buffer felső korlát (4 KB) */
#define BKPT_INSTR      0xBE00BE00u  /* két Thumb BKPT (32-bites szóként) */

/* CMSIS FlashOS Init fnc kódok. */
#define FNC_ERASE   1u
#define FNC_PROGRAM 2u
#define FNC_VERIFY  3u

/* A betöltött algo + kiszámolt címek statikus állapota. */
static struct {
    bool             loaded;
    const flm_algo_t *algo;
    uint32_t         code_base;     /* PrgCode kezdőcíme (= RAM_BASE) */
    uint32_t         data_base;     /* PrgData a kód mögött */
    uint32_t         bkpt_addr;     /* BKPT-szó címe (call visszatérés) */
    uint32_t         stack_top;     /* SP induló értéke (8-byte align) */
    uint32_t         buf_addr;      /* page-buffer címe a cél RAM-jában */
    uint32_t         buf_size;      /* használható buffer méret */
} s_state;

/* --- segéd: byte-buffer a célba 32-bites szavakban, a végén lévő
 *     részleges (1..3 bájt) szó kezelésével. A forrás lehet igazítatlan. --- */
static esp_err_t write_mem(uint32_t addr, const uint8_t *data, size_t len)
{
    size_t full_words = len / 4u;
    enum { CHUNK = 64 };
    uint32_t tmp[CHUNK];
    size_t done = 0;
    while (done < full_words) {
        size_t n = full_words - done;
        if (n > CHUNK) n = CHUNK;
        memcpy(tmp, data + done * 4u, n * 4u);  /* igazítatlan forrás kezelése */
        esp_err_t e = adiv5_write_block(addr + (uint32_t)(done * 4u), tmp, n);
        if (e != ESP_OK) return e;
        done += n;
    }
    size_t rem = len & 3u;
    if (rem) {
        /* Maradék 1..3 bájt 0-val kiegészítve egy teljes szóba. */
        uint32_t w = 0;
        memcpy(&w, data + full_words * 4u, rem);
        esp_err_t e = adiv5_write32(addr + (uint32_t)(full_words * 4u), w);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}

esp_err_t flm_runner_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    ESP_LOGI(TAG, "flm_runner init");
    return ESP_OK;
}

esp_err_t flm_runner_load(const flm_algo_t *algo)
{
    if (!algo || !algo->prg_code || algo->prg_code_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* --- címszámítás 8-bájtos igazítással --- */
    uint32_t code_base = RAM_BASE;
    uint32_t data_base = ALIGN8(code_base + algo->prg_code_len);
    uint32_t bkpt_addr = ALIGN8(data_base + algo->prg_data_len);
    uint32_t stack_bot = bkpt_addr + 8u;              /* BKPT-szó után */
    uint32_t stack_top = ALIGN8(stack_bot + STACK_SIZE);
    uint32_t buf_addr  = ALIGN8(stack_top);           /* buffer a stack fölött */

    /* Buffer méret: page_size-hoz méretezve, a felső korlát alatt. */
    uint32_t buf_size = algo->page_size ? algo->page_size : BUFFER_SIZE;
    if (buf_size > BUFFER_SIZE) buf_size = BUFFER_SIZE;

    ESP_LOGI(TAG, "RAM layout: code=0x%08lx data=0x%08lx bkpt=0x%08lx sp=0x%08lx buf=0x%08lx(%lu)",
             (unsigned long)code_base, (unsigned long)data_base,
             (unsigned long)bkpt_addr, (unsigned long)stack_top,
             (unsigned long)buf_addr, (unsigned long)buf_size);

    /* --- PrgCode betöltése a cél RAM-jába (blokkos + záró részleges szó) --- */
    esp_err_t e = write_mem(code_base, algo->prg_code, algo->prg_code_len);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "PrgCode betöltés hiba: %d", e);
        return e;
    }

    /* --- BKPT-szó kiírása a visszatérési pontra --- */
    e = adiv5_write32(bkpt_addr, BKPT_INSTR);
    if (e != ESP_OK) return e;

    s_state.loaded    = true;
    s_state.algo      = algo;
    s_state.code_base = code_base;
    s_state.data_base = data_base;
    s_state.bkpt_addr = bkpt_addr;
    s_state.stack_top = stack_top;
    s_state.buf_addr  = buf_addr;
    s_state.buf_size  = buf_size;
    return ESP_OK;
}

esp_err_t flm_runner_call(uint32_t func_off,
                          uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
                          uint32_t timeout_ms, int *ret)
{
    if (!s_state.loaded) return ESP_ERR_INVALID_STATE;

    uint32_t pc = s_state.code_base + func_off;

    /* --- call_function ABI (terv 8.) --- */
    esp_err_t e;
    if ((e = cortexm_reg_write(CM_REG_R0 + 0, r0)) != ESP_OK) return e;
    if ((e = cortexm_reg_write(CM_REG_R0 + 1, r1)) != ESP_OK) return e;
    if ((e = cortexm_reg_write(CM_REG_R0 + 2, r2)) != ESP_OK) return e;
    if ((e = cortexm_reg_write(CM_REG_R0 + 3, r3)) != ESP_OK) return e;
    /* SP 8-byte igazítva. */
    if ((e = cortexm_reg_write(CM_REG_SP, s_state.stack_top & ~7u)) != ESP_OK) return e;
    /* LR -> BKPT, Thumb bit (visszatéréskor a BKPT-re ugrik). */
    if ((e = cortexm_reg_write(CM_REG_LR, s_state.bkpt_addr | 1u)) != ESP_OK) return e;
    /* PC -> a függvény, Thumb bit. */
    if ((e = cortexm_reg_write(CM_REG_PC, pc | 1u)) != ESP_OK) return e;
    /* xPSR T-bit (enélkül INVSTATE usage fault). */
    if ((e = cortexm_reg_write(CM_REG_XPSR, 0x01000000u)) != ESP_OK) return e;

    /* A BKPT-szót a load() már kiírta, de minden hívás előtt megerősítjük. */
    if ((e = adiv5_write32(s_state.bkpt_addr, BKPT_INSTR)) != ESP_OK) return e;

    /* --- indítás + várakozás halt-ra --- */
    if ((e = cortexm_resume()) != ESP_OK) return e;
    if ((e = cortexm_wait_halt(timeout_ms)) != ESP_OK) {
        ESP_LOGE(TAG, "call_function timeout/halt hiba @0x%08lx: %d",
                 (unsigned long)pc, e);
        return e;
    }

    /* Opcionális ellenőrzés: tényleg a BKPT-nél állt-e meg? (csak warning) */
    uint32_t stopped_pc = 0;
    if (cortexm_reg_read(CM_REG_PC, &stopped_pc) == ESP_OK) {
        if ((stopped_pc & ~1u) != (s_state.bkpt_addr & ~1u)) {
            ESP_LOGW(TAG, "halt nem a BKPT-nél: pc=0x%08lx (várt 0x%08lx)",
                     (unsigned long)stopped_pc, (unsigned long)s_state.bkpt_addr);
        }
    }

    /* --- visszatérési érték R0-ból --- */
    uint32_t rv = 0;
    if ((e = cortexm_reg_read(CM_REG_R0, &rv)) != ESP_OK) return e;
    if (ret) *ret = (int)rv;
    return ESP_OK;
}

/* Belső segéd: Init(addr, clk, fnc), 0=siker. off_init==0 -> skip. */
static esp_err_t call_init(const flm_algo_t *algo, uint32_t base, uint32_t fnc)
{
    if (algo->off_init == 0) return ESP_OK;
    int rc = -1;
    esp_err_t e = flm_runner_call(algo->off_init, base, 0 /*clk*/, fnc, 0,
                                  algo->timeout_prog_ms, &rc);
    if (e != ESP_OK) return e;
    if (rc != 0) {
        ESP_LOGE(TAG, "Init(fnc=%lu) hiba rc=%d", (unsigned long)fnc, rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Belső segéd: UnInit(fnc), 0=siker. off_uninit==0 -> skip.
 * Az UnInit hibája nem feltétlen végzetes -> csak warning. */
static esp_err_t call_uninit(const flm_algo_t *algo, uint32_t fnc)
{
    if (algo->off_uninit == 0) return ESP_OK;
    int rc = -1;
    esp_err_t e = flm_runner_call(algo->off_uninit, fnc, 0, 0, 0,
                                  algo->timeout_prog_ms, &rc);
    if (e != ESP_OK) return e;
    if (rc != 0) {
        ESP_LOGW(TAG, "UnInit(fnc=%lu) rc=%d", (unsigned long)fnc, rc);
    }
    return ESP_OK;
}

esp_err_t flm_runner_program(const flm_algo_t *algo, uint32_t base_addr,
                             const uint8_t *data, size_t len,
                             flm_progress_cb cb, void *ctx)
{
    if (!algo || !data || len == 0) return ESP_ERR_INVALID_ARG;

    /* Betöltés, ha még nem ez az algo van a cél RAM-jában. */
    if (!s_state.loaded || s_state.algo != algo) {
        esp_err_t e = flm_runner_load(algo);
        if (e != ESP_OK) return e;
    }

    esp_err_t e;
    int rc;
    uint32_t page = algo->page_size ? algo->page_size : 256u;
    if (page > s_state.buf_size) page = s_state.buf_size;

    /* =========================== ERASE fázis =========================== */
    e = call_init(algo, algo->dev_addr, FNC_ERASE);
    if (e != ESP_OK) return e;

    if (algo->off_erase_chip != 0) {
        /* Teljes chip törlés egy lépésben. */
        rc = -1;
        e = flm_runner_call(algo->off_erase_chip, 0, 0, 0, 0,
                            algo->timeout_erase_ms, &rc);
        if (e == ESP_OK && rc != 0) e = ESP_FAIL;
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "EraseChip hiba rc=%d e=%d", rc, e);
            call_uninit(algo, FNC_ERASE);
            return ESP_FAIL;
        }
        if (cb) cb("Erase", (uint32_t)len, (uint32_t)len, ctx);
    } else if (algo->off_erase_sector != 0) {
        /* Szektoronkénti törlés a [base_addr, base_addr+len) tartományra.
           A szektorméret a sectors[] táblából, az eszköz elejéhez relatívan. */
        uint32_t region_end = base_addr + (uint32_t)len;
        uint32_t a = base_addr;
        if (cb) cb("Erase", 0, (uint32_t)len, ctx);
        while (a < region_end) {
            /* Az aktuális címhez tartozó szektorméret meghatározása. */
            uint32_t sec_size = 0;
            uint32_t rel = a - algo->dev_addr;  /* eszköz elejéhez képest */
            if (algo->sectors && algo->sector_count) {
                /* sectors[] növekvő addr szerint: az utolsó addr<=rel elem
                   mérete érvényes innentől. */
                for (uint32_t i = 0; i < algo->sector_count; i++) {
                    if (algo->sectors[i].addr <= rel) {
                        sec_size = algo->sectors[i].size;
                    } else {
                        break;
                    }
                }
            }
            if (sec_size == 0) sec_size = page;  /* fallback */

            rc = -1;
            e = flm_runner_call(algo->off_erase_sector, a, 0, 0, 0,
                                algo->timeout_erase_ms, &rc);
            if (e == ESP_OK && rc != 0) e = ESP_FAIL;
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "EraseSector(0x%08lx) hiba rc=%d e=%d",
                         (unsigned long)a, rc, e);
                call_uninit(algo, FNC_ERASE);
                return ESP_FAIL;
            }
            a += sec_size;
            uint32_t done = (a > region_end ? region_end : a) - base_addr;
            if (cb) cb("Erase", done, (uint32_t)len, ctx);
        }
    } else {
        ESP_LOGW(TAG, "nincs EraseChip/EraseSector ABI -> erase kihagyva");
        if (cb) cb("Erase", (uint32_t)len, (uint32_t)len, ctx);
    }

    e = call_uninit(algo, FNC_ERASE);
    if (e != ESP_OK) return e;

    /* ========================== PROGRAM fázis ========================== */
    if (algo->off_program_page != 0) {
        e = call_init(algo, algo->dev_addr, FNC_PROGRAM);
        if (e != ESP_OK) return e;

        if (cb) cb("Program", 0, (uint32_t)len, ctx);
        size_t off = 0;
        while (off < len) {
            size_t chunk = len - off;
            if (chunk > page) chunk = page;

            /* A page-nyi adat a cél RAM bufferébe. */
            e = write_mem(s_state.buf_addr, data + off, chunk);
            if (e != ESP_OK) { call_uninit(algo, FNC_PROGRAM); return e; }

            /* ProgramPage(adr, sz, buf). */
            rc = -1;
            e = flm_runner_call(algo->off_program_page,
                                base_addr + (uint32_t)off,
                                (uint32_t)chunk,
                                s_state.buf_addr, 0,
                                algo->timeout_prog_ms, &rc);
            if (e == ESP_OK && rc != 0) e = ESP_FAIL;
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "ProgramPage(0x%08lx,%u) hiba rc=%d e=%d",
                         (unsigned long)(base_addr + off),
                         (unsigned)chunk, rc, e);
                call_uninit(algo, FNC_PROGRAM);
                return ESP_FAIL;
            }

            off += chunk;
            if (cb) cb("Program", (uint32_t)off, (uint32_t)len, ctx);
        }

        e = call_uninit(algo, FNC_PROGRAM);
        if (e != ESP_OK) return e;
    } else {
        ESP_LOGW(TAG, "nincs ProgramPage ABI -> programozás kihagyva");
    }

    /* =========================== VERIFY fázis ========================== */
    if (algo->off_verify != 0) {
        /* FLM Verify(adr, sz, buf): siker esetén adr+sz a visszatérés. */
        e = call_init(algo, algo->dev_addr, FNC_VERIFY);
        if (e != ESP_OK) return e;

        if (cb) cb("Verify", 0, (uint32_t)len, ctx);
        size_t off = 0;
        while (off < len) {
            size_t chunk = len - off;
            if (chunk > page) chunk = page;

            e = write_mem(s_state.buf_addr, data + off, chunk);
            if (e != ESP_OK) { call_uninit(algo, FNC_VERIFY); return e; }

            uint32_t expect = (base_addr + (uint32_t)off) + (uint32_t)chunk;
            rc = -1;
            e = flm_runner_call(algo->off_verify,
                                base_addr + (uint32_t)off,
                                (uint32_t)chunk,
                                s_state.buf_addr, 0,
                                algo->timeout_prog_ms, &rc);
            if (e != ESP_OK) { call_uninit(algo, FNC_VERIFY); return e; }
            if ((uint32_t)rc != expect) {
                ESP_LOGE(TAG, "Verify eltérés @0x%08lx: rc=0x%08lx várt=0x%08lx",
                         (unsigned long)(base_addr + off),
                         (unsigned long)(uint32_t)rc, (unsigned long)expect);
                call_uninit(algo, FNC_VERIFY);
                return ESP_FAIL;
            }

            off += chunk;
            if (cb) cb("Verify", (uint32_t)off, (uint32_t)len, ctx);
        }

        e = call_uninit(algo, FNC_VERIFY);
        if (e != ESP_OK) return e;
    } else {
        /* Visszaolvasásos ellenőrzés adiv5_read_block-kal. */
        ESP_LOGI(TAG, "nincs Verify ABI -> visszaolvasásos ellenőrzés");
        if (cb) cb("Verify", 0, (uint32_t)len, ctx);
        size_t off = 0;
        while (off < len) {
            size_t chunk = len - off;
            if (chunk > 64u) chunk = 64u;
            /* 32-bites igazítású blokkos olvasás, majd bájtos összevetés. */
            uint32_t words[16];
            size_t nwords = (chunk + 3u) / 4u;
            e = adiv5_read_block(base_addr + (uint32_t)off, words, nwords);
            if (e != ESP_OK) return e;
            if (memcmp(words, data + off, chunk) != 0) {
                ESP_LOGE(TAG, "visszaolvasás eltérés @0x%08lx",
                         (unsigned long)(base_addr + off));
                return ESP_FAIL;
            }
            off += chunk;
            if (cb) cb("Verify", (uint32_t)off, (uint32_t)len, ctx);
        }
    }

    ESP_LOGI(TAG, "programozás kész: 0x%08lx +%u byte",
             (unsigned long)base_addr, (unsigned)len);
    return ESP_OK;
}
