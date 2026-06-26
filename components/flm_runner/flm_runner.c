/* Flash-loader futtató implementáció — KÉT ABI támogatással. (terv 8-9. szekció)
 *
 * RAM-elrendezés a cél SRAM-jában (a betöltött kép FÖLÖTT):
 *   [code @ load_addr ...][BKPT-szó][stack ↓][buffer]
 *
 * A `code`-ot a `load_addr` ABSZOLÚT címére töltjük (CMSIS: 0x20000000,
 * ST: a section saját abszolút címe). A munka-régiókat (BKPT-szó, stack,
 * buffer) a betöltött kép VÉGE (image_end = load_addr + code_len) fölött
 * számoljuk ki. A BKPT-szó a call_function visszatérési pontja (LR ide
 * mutat), a stack a BKPT fölött nő, a buffer pedig a stack teteje fölött
 * kap helyet (ide kerül a programozandó adat a célban).
 *
 * Csak a cortexm_debug + adiv5 rétegre épül.
 *
 * --- ABI eltérések (lásd flm_blobs.h) ---
 *   FLM_ABI_CMSIS: 0=siker, off_* a load_addr-hoz RELATÍV, az abszolút PC a
 *     load_addr + off. Init(adr,clk,fnc)/EraseSector(adr)/ProgramPage(adr,sz,buf)/
 *     Verify(adr,sz,buf).
 *   FLM_ABI_ST: 1=siker, off_* már ABSZOLÚT cím (Thumb-bit nélkül). Init(void)/
 *     Write(addr,size,buf)/SectorErase(start,end)/MassErase(void)/Verify(addr,size,buf).
 */
#include <string.h>
#include "esp_log.h"

#include "flm_runner.h"
#include "flm_blobs.h"
#include "cortexm_debug.h"
#include "adiv5.h"

static const char *TAG = "flm_runner";

/* --- RAM-elrendezés alapértékek --- */
#define ALIGN8(x)       (((x) + 7u) & ~7u)
#define STACK_SIZE      0x400u       /* 1 KB stack a FLM rutinoknak */
#define BUFFER_MAX      0x1000u      /* page-buffer felső korlát (4 KB) */
#define BKPT_INSTR      0xBE00BE00u  /* két Thumb BKPT (32-bites szóként) */

/* CMSIS FlashOS Init fnc kódok. */
#define FNC_ERASE   1u
#define FNC_PROGRAM 2u
#define FNC_VERIFY  3u

/* A betöltött algo + kiszámolt címek statikus állapota. */
static struct {
    bool             loaded;
    const flm_algo_t *algo;
    uint32_t         load_addr;     /* a kód betöltési címe (= algo->load_addr) */
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
    if (!algo || !algo->code || algo->code_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* --- címszámítás a betöltött kép FÖLÖTT, 8-bájtos igazítással --- */
    uint32_t load_addr = algo->load_addr;
    uint32_t image_end = ALIGN8(load_addr + algo->code_len); /* kép vége 8-ra igazítva */
    uint32_t bkpt_addr = image_end;                          /* BKPT-szó a kép után */
    uint32_t stack_top = ALIGN8(bkpt_addr + 8u + STACK_SIZE);/* BKPT-szó + stack */
    uint32_t buf_addr  = ALIGN8(stack_top);                  /* buffer a stack fölött */

    /* Buffer méret: page_size-ra kerekítve, a felső korlát alatt. */
    uint32_t page = algo->page_size ? algo->page_size : 256u;
    uint32_t buf_size = ALIGN8(page);
    if (buf_size > BUFFER_MAX) buf_size = BUFFER_MAX;

    ESP_LOGI(TAG, "RAM layout: load=0x%08lx end=0x%08lx bkpt=0x%08lx sp=0x%08lx buf=0x%08lx(%lu)",
             (unsigned long)load_addr, (unsigned long)image_end,
             (unsigned long)bkpt_addr, (unsigned long)stack_top,
             (unsigned long)buf_addr, (unsigned long)buf_size);

    /* --- code betöltése a cél RAM-jába (blokkos + záró részleges szó) --- */
    esp_err_t e = write_mem(load_addr, algo->code, algo->code_len);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "code betöltés hiba: %d", e);
        return e;
    }

    /* --- BKPT-szó kiírása a visszatérési pontra --- */
    e = adiv5_write32(bkpt_addr, BKPT_INSTR);
    if (e != ESP_OK) return e;

    s_state.loaded    = true;
    s_state.algo      = algo;
    s_state.load_addr = load_addr;
    s_state.bkpt_addr = bkpt_addr;
    s_state.stack_top = stack_top;
    s_state.buf_addr  = buf_addr;
    s_state.buf_size  = buf_size;
    return ESP_OK;
}

/* --- belső, közös hívó: ABSZOLÚT PC-vel hív (mindkét ABI ezt használja).
 *     reg R0..R3, SP=stack_top, LR=bkpt|1, PC=pc_abs|1, xPSR T-bit, BKPT-szó,
 *     resume + wait_halt, majd ret = R0. --- */
static esp_err_t flm_call_pc(uint32_t pc_abs,
                             uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
                             uint32_t timeout_ms, int *ret)
{
    if (!s_state.loaded) return ESP_ERR_INVALID_STATE;

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
    /* PC -> a függvény ABSZOLÚT címe, Thumb bit. */
    if ((e = cortexm_reg_write(CM_REG_PC, pc_abs | 1u)) != ESP_OK) return e;
    /* xPSR T-bit (enélkül INVSTATE usage fault). */
    if ((e = cortexm_reg_write(CM_REG_XPSR, 0x01000000u)) != ESP_OK) return e;

    /* A BKPT-szót a load() már kiírta, de minden hívás előtt megerősítjük. */
    if ((e = adiv5_write32(s_state.bkpt_addr, BKPT_INSTR)) != ESP_OK) return e;

    /* --- indítás + várakozás halt-ra --- */
    if ((e = cortexm_resume()) != ESP_OK) return e;
    if ((e = cortexm_wait_halt(timeout_ms)) != ESP_OK) {
        ESP_LOGE(TAG, "call_function timeout/halt hiba @0x%08lx: %d",
                 (unsigned long)pc_abs, e);
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

/* Publikus hívó: CMSIS-szemantika — a func_off a load_addr-hoz RELATÍV,
 * az abszolút PC = load_addr + func_off. */
esp_err_t flm_runner_call(uint32_t func_off,
                          uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
                          uint32_t timeout_ms, int *ret)
{
    if (!s_state.loaded) return ESP_ERR_INVALID_STATE;
    return flm_call_pc(s_state.load_addr + func_off, r0, r1, r2, r3, timeout_ms, ret);
}

/* =====================================================================
 *  CMSIS ABI segédek (off_* a load_addr-hoz relatív, 0=siker, success_ret).
 * ===================================================================== */

/* Init(addr, clk, fnc). off_init==0 -> skip. */
static esp_err_t cmsis_init(const flm_algo_t *algo, uint32_t base, uint32_t fnc)
{
    if (algo->off_init == 0) return ESP_OK;
    int rc = -1;
    esp_err_t e = flm_call_pc(algo->load_addr + algo->off_init,
                              base, 0 /*clk*/, fnc, 0,
                              algo->timeout_prog_ms, &rc);
    if (e != ESP_OK) return e;
    if (rc != algo->success_ret) {
        ESP_LOGE(TAG, "CMSIS Init(fnc=%lu) hiba rc=%d", (unsigned long)fnc, rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* UnInit(fnc). off_uninit==0 -> skip. Hibája nem végzetes -> csak warning. */
static esp_err_t cmsis_uninit(const flm_algo_t *algo, uint32_t fnc)
{
    if (algo->off_uninit == 0) return ESP_OK;
    int rc = -1;
    esp_err_t e = flm_call_pc(algo->load_addr + algo->off_uninit,
                              fnc, 0, 0, 0,
                              algo->timeout_prog_ms, &rc);
    if (e != ESP_OK) return e;
    if (rc != algo->success_ret) {
        ESP_LOGW(TAG, "CMSIS UnInit(fnc=%lu) rc=%d", (unsigned long)fnc, rc);
    }
    return ESP_OK;
}

static esp_err_t program_cmsis(const flm_algo_t *algo, uint32_t base_addr,
                               const uint8_t *data, size_t len,
                               flm_progress_cb cb, void *ctx)
{
    esp_err_t e;
    int rc;
    uint32_t page = algo->page_size ? algo->page_size : 256u;
    if (page > s_state.buf_size) page = s_state.buf_size;

    /* =========================== ERASE fázis =========================== */
    e = cmsis_init(algo, algo->dev_addr, FNC_ERASE);
    if (e != ESP_OK) return e;

    if (algo->off_erase_chip != 0) {
        /* Teljes chip törlés egy lépésben. */
        rc = -1;
        e = flm_call_pc(algo->load_addr + algo->off_erase_chip, 0, 0, 0, 0,
                        algo->timeout_erase_ms, &rc);
        if (e == ESP_OK && rc != algo->success_ret) e = ESP_FAIL;
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "EraseChip hiba rc=%d e=%d", rc, e);
            cmsis_uninit(algo, FNC_ERASE);
            return ESP_FAIL;
        }
        if (cb) cb("Erase", (uint32_t)len, (uint32_t)len, ctx);
    } else if (algo->off_erase_sector != 0) {
        /* Szektoronkénti törlés a [base_addr, base_addr+len) tartományra. */
        uint32_t region_end = base_addr + (uint32_t)len;
        uint32_t a = base_addr;
        if (cb) cb("Erase", 0, (uint32_t)len, ctx);
        while (a < region_end) {
            uint32_t sec_size = 0;
            uint32_t rel = a - algo->dev_addr;  /* eszköz elejéhez képest */
            if (algo->sectors && algo->sector_count) {
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
            e = flm_call_pc(algo->load_addr + algo->off_erase_sector, a, 0, 0, 0,
                            algo->timeout_erase_ms, &rc);
            if (e == ESP_OK && rc != algo->success_ret) e = ESP_FAIL;
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "EraseSector(0x%08lx) hiba rc=%d e=%d",
                         (unsigned long)a, rc, e);
                cmsis_uninit(algo, FNC_ERASE);
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

    e = cmsis_uninit(algo, FNC_ERASE);
    if (e != ESP_OK) return e;

    /* ========================== PROGRAM fázis ========================== */
    if (algo->off_program_page != 0) {
        e = cmsis_init(algo, algo->dev_addr, FNC_PROGRAM);
        if (e != ESP_OK) return e;

        if (cb) cb("Program", 0, (uint32_t)len, ctx);
        size_t off = 0;
        while (off < len) {
            size_t chunk = len - off;
            if (chunk > page) chunk = page;

            e = write_mem(s_state.buf_addr, data + off, chunk);
            if (e != ESP_OK) { cmsis_uninit(algo, FNC_PROGRAM); return e; }

            rc = -1;
            e = flm_call_pc(algo->load_addr + algo->off_program_page,
                            base_addr + (uint32_t)off,
                            (uint32_t)chunk,
                            s_state.buf_addr, 0,
                            algo->timeout_prog_ms, &rc);
            if (e == ESP_OK && rc != algo->success_ret) e = ESP_FAIL;
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "ProgramPage(0x%08lx,%u) hiba rc=%d e=%d",
                         (unsigned long)(base_addr + off),
                         (unsigned)chunk, rc, e);
                cmsis_uninit(algo, FNC_PROGRAM);
                return ESP_FAIL;
            }

            off += chunk;
            if (cb) cb("Program", (uint32_t)off, (uint32_t)len, ctx);
        }

        e = cmsis_uninit(algo, FNC_PROGRAM);
        if (e != ESP_OK) return e;
    } else {
        ESP_LOGW(TAG, "nincs ProgramPage ABI -> programozás kihagyva");
    }

    /* =========================== VERIFY fázis ========================== */
    if (algo->off_verify != 0) {
        /* FLM Verify(adr, sz, buf): siker esetén adr+sz a visszatérés. */
        e = cmsis_init(algo, algo->dev_addr, FNC_VERIFY);
        if (e != ESP_OK) return e;

        if (cb) cb("Verify", 0, (uint32_t)len, ctx);
        size_t off = 0;
        while (off < len) {
            size_t chunk = len - off;
            if (chunk > page) chunk = page;

            e = write_mem(s_state.buf_addr, data + off, chunk);
            if (e != ESP_OK) { cmsis_uninit(algo, FNC_VERIFY); return e; }

            uint32_t expect = (base_addr + (uint32_t)off) + (uint32_t)chunk;
            rc = -1;
            e = flm_call_pc(algo->load_addr + algo->off_verify,
                            base_addr + (uint32_t)off,
                            (uint32_t)chunk,
                            s_state.buf_addr, 0,
                            algo->timeout_prog_ms, &rc);
            if (e != ESP_OK) { cmsis_uninit(algo, FNC_VERIFY); return e; }
            if ((uint32_t)rc != expect) {
                ESP_LOGE(TAG, "Verify eltérés @0x%08lx: rc=0x%08lx várt=0x%08lx",
                         (unsigned long)(base_addr + off),
                         (unsigned long)(uint32_t)rc, (unsigned long)expect);
                cmsis_uninit(algo, FNC_VERIFY);
                return ESP_FAIL;
            }

            off += chunk;
            if (cb) cb("Verify", (uint32_t)off, (uint32_t)len, ctx);
        }

        e = cmsis_uninit(algo, FNC_VERIFY);
        if (e != ESP_OK) return e;
    } else {
        /* Visszaolvasásos ellenőrzés adiv5_read_block-kal. */
        ESP_LOGI(TAG, "nincs Verify ABI -> visszaolvasásos ellenőrzés");
        if (cb) cb("Verify", 0, (uint32_t)len, ctx);
        size_t off = 0;
        while (off < len) {
            size_t chunk = len - off;
            if (chunk > 64u) chunk = 64u;
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

    return ESP_OK;
}

/* =====================================================================
 *  ST ABI segédek (off_* ABSZOLÚT cím, 1=siker, success_ret).
 *  Init(void) / SectorErase(start,end) / MassErase(void) /
 *  Write(addr,size,buf) / Verify(addr,size,buf).
 * ===================================================================== */
static esp_err_t program_st(const flm_algo_t *algo, uint32_t base_addr,
                            const uint8_t *data, size_t len,
                            flm_progress_cb cb, void *ctx)
{
    esp_err_t e;
    int rc;
    uint32_t page = algo->page_size ? algo->page_size : 256u;
    if (page > s_state.buf_size) page = s_state.buf_size;

    /* ============================= 1. Init ============================= */
    /* ST Init(void); a felesleges 0 argumentumok ártalmatlanok. */
    if (algo->off_init != 0) {
        rc = -1;
        e = flm_call_pc(algo->off_init, 0, 0, 0, 0, algo->timeout_prog_ms, &rc);
        if (e != ESP_OK) return e;
        if (rc != algo->success_ret) {
            ESP_LOGE(TAG, "ST Init hiba rc=%d (várt %d)", rc, algo->success_ret);
            return ESP_FAIL;
        }
    }

    /* ============================= 2. Erase ============================ */
    /* SectorErase(start, end): abszolút flash-címek, egyetlen tartomány. */
    if (algo->off_st_sector_erase != 0) {
        uint32_t start = base_addr;
        uint32_t end   = base_addr + (uint32_t)len - 1u;
        if (cb) cb("Erase", 0, (uint32_t)len, ctx);
        rc = -1;
        e = flm_call_pc(algo->off_st_sector_erase, start, end, 0, 0,
                        algo->timeout_erase_ms, &rc);
        if (e != ESP_OK) return e;
        if (rc != algo->success_ret) {
            ESP_LOGE(TAG, "ST SectorErase(0x%08lx..0x%08lx) hiba rc=%d",
                     (unsigned long)start, (unsigned long)end, rc);
            return ESP_FAIL;
        }
        if (cb) cb("Erase", (uint32_t)len, (uint32_t)len, ctx);
    } else if (algo->off_st_mass_erase != 0) {
        if (cb) cb("Erase", 0, (uint32_t)len, ctx);
        rc = -1;
        e = flm_call_pc(algo->off_st_mass_erase, 0, 0, 0, 0,
                        algo->timeout_erase_ms, &rc);
        if (e != ESP_OK) return e;
        if (rc != algo->success_ret) {
            ESP_LOGE(TAG, "ST MassErase hiba rc=%d", rc);
            return ESP_FAIL;
        }
        if (cb) cb("Erase", (uint32_t)len, (uint32_t)len, ctx);
    } else {
        ESP_LOGW(TAG, "nincs ST SectorErase/MassErase -> erase kihagyva");
        if (cb) cb("Erase", (uint32_t)len, (uint32_t)len, ctx);
    }

    /* ============================ 3. Program =========================== */
    /* Write(flash_addr, chunk_size, buf_addr): a chunk a cél-RAM bufferében. */
    if (algo->off_st_write != 0) {
        if (cb) cb("Program", 0, (uint32_t)len, ctx);
        size_t off = 0;
        while (off < len) {
            size_t chunk = len - off;
            if (chunk > page) chunk = page;

            e = write_mem(s_state.buf_addr, data + off, chunk);
            if (e != ESP_OK) return e;

            uint32_t flash_addr = base_addr + (uint32_t)off;
            rc = -1;
            e = flm_call_pc(algo->off_st_write,
                            flash_addr, (uint32_t)chunk, s_state.buf_addr, 0,
                            algo->timeout_prog_ms, &rc);
            if (e != ESP_OK) return e;
            if (rc != algo->success_ret) {
                ESP_LOGE(TAG, "ST Write(0x%08lx,%u) hiba rc=%d",
                         (unsigned long)flash_addr, (unsigned)chunk, rc);
                return ESP_FAIL;
            }

            off += chunk;
            if (cb) cb("Program", (uint32_t)off, (uint32_t)len, ctx);
        }
    } else {
        ESP_LOGW(TAG, "nincs ST Write ABI -> programozás kihagyva");
    }

    /* ============================ 4. Verify =========================== */
    /* Az ST Verify visszatérése implementáció-függő (jellemzően addr+size
     * vagy checksum), így megbízhatatlan az általános success_ret-alapú
     * ellenőrzéshez. Ezért — a feladat ajánlásának megfelelően — a biztos
     * úton megyünk: adiv5_read_block visszaolvasás + memcmp a `data`-val.
     * Ez determinisztikus és nem függ az adott loader Verify konvenciójától. */
    ESP_LOGI(TAG, "ST verify: visszaolvasásos ellenőrzés (read_block + memcmp)");
    if (cb) cb("Verify", 0, (uint32_t)len, ctx);
    {
        size_t off = 0;
        while (off < len) {
            size_t chunk = len - off;
            if (chunk > 64u) chunk = 64u;
            uint32_t words[16];
            size_t nwords = (chunk + 3u) / 4u;
            e = adiv5_read_block(base_addr + (uint32_t)off, words, nwords);
            if (e != ESP_OK) return e;
            if (memcmp(words, data + off, chunk) != 0) {
                ESP_LOGE(TAG, "ST visszaolvasás eltérés @0x%08lx",
                         (unsigned long)(base_addr + off));
                return ESP_FAIL;
            }
            off += chunk;
            if (cb) cb("Verify", (uint32_t)off, (uint32_t)len, ctx);
        }
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
    switch (algo->abi) {
    case FLM_ABI_ST:
        e = program_st(algo, base_addr, data, len, cb, ctx);
        break;
    case FLM_ABI_CMSIS:
    default:
        e = program_cmsis(algo, base_addr, data, len, cb, ctx);
        break;
    }
    if (e != ESP_OK) return e;

    ESP_LOGI(TAG, "programozás kész: 0x%08lx +%u byte (abi=%d)",
             (unsigned long)base_addr, (unsigned)len, (int)algo->abi);
    return ESP_OK;
}
