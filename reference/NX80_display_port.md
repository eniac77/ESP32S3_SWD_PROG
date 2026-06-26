# NX80TESTER (AVR) kijelző- és bevitel-kód → ESP-IDF port-terv

**Forrás:** `d:\--- CLAUDE ---\NX80TESTER_AVR\` (AVR0/1-széria, `TWI0` I2C master, `PORTF` enkóder ISR)
**Cél:** `d:\--- CLAUDE ---\ESP32S3_SWD_PROG\` — `components/display_oled/` és `components/input_enc/`
**Cél HW:** ESP32-S3-N16R8, OLED I2C0 @ 400 kHz (SDA=GPIO8, SCL=GPIO9), enkóder A=GPIO10, B=GPIO11, SW=GPIO12.
**Hivatkozott terv:** `ESP32S3_SWD_PROG_Plan.md` 14. (`input_enc`) és 15. (`display_oled`) szekció.

> Ez a dokumentum **csak elemzés és terv** — az AVR forrás módosítatlan marad.

---

## 0. Az AVR kód térképe (réteges felépítés)

```
app.c            ── UI állapotgép + render-loop (ST_MAIN/RUN/ERROR/MENU)   [AVR-specifikus logika, de jól portolható minta]
 ├─ display.c    ── magasszintű képernyők (screen_*) + 5x7 szövegrajz       [PLATFORMFÜGGETLEN]
 │   ├─ font.c   ── 5x7 ASCII font (0x20..0x5F), pgmspace tömb              [PLATFORMFÜGGETLEN ADAT]
 │   └─ mikrofont.c + font_swis721.h ── nagy 19x24 szám-font renderer       [PLATFORMFÜGGETLEN ADAT + LOGIKA]
 └─ ssd1306.c    ── framebuffer (1024 B) + rajz-primitívek + init + flush
     ├─ fb[], pixel/rect/fill_rect, clear                                   [PLATFORMFÜGGETLEN]
     ├─ init szekvencia (seq[])                                            [PANEL-FÜGGŐ ADAT, hordozható]
     └─ cmd()/flush() → twi_*                                              [AVR-SPECIFIKUS I/O]
        twi.c     ── TWI0 regiszteres I2C master                            [AVR-SPECIFIKUS — újraírandó]

encoder.c        ── kvadratúra állapotgép, PORTF ISR                        [LOGIKA hordozható, ISR AVR-specifikus]
button.c         ── poll-alapú debounce + short/long                       [LOGIKA hordozható, port-olvasás AVR-specifikus]
gpio.c           ── láb-init, kimenetek, LED-ek, error-bemenet             [AVR-SPECIFIKUS — a tester app-hoz tartozik, nem kell]
config.h         ── F_CPU, időzítési konstansok (ms)                        [konstansok átemelhetők]
```

---

## 1. Újrahasznosítható assetek

### 1.1 Platformfüggetlen (szinte változatlanul átemelhető)

| Asset | Fájl:sor | Mit ad | Megjegyzés |
|---|---|---|---|
| **5x7 ASCII font** | `font.c:6-71` | 96 glyph, 0x20..0x5F, 5 oszlop/karakter, oszloponként alsó bit = felső pixel | Csak a `PROGMEM`/`pgm_read_byte` (`font.c:3,82`) AVR-specifikus → sima `const` tömb + indexelés. |
| **Nagy szám-font (Swis721 19x24)** | `font_swis721.h:23-37` | `'.'`,`'/'`,`0`–`9`,`':'` (char 46..58), MikroElektronika GLCD formátum, oszlop-alapú, byte-onként 8 sor, fejléc-byte = változó szélesség | Csak `.`,`/`,számok,`:` van benne — pont a `counter.` és `MM:SS` kiíráshoz. |
| **Nagy-font renderer** | `mikrofont.c:37-91` | `bigfont_char/str/str_width/center` — variábilis szélesség, framebufferbe rajzol | Logika hordozható; csak `pgm_read_byte` cseréje + `ssd1306_pixel` átnevezés. |
| **Framebuffer + rajz-primitívek** | `ssd1306.c:11,56-116` | `fb[1024]`, `clear`, `pixel`, `fill_rect`, `rect` (1px keret) | Tisztán memória-művelet, nulla HW-függés. |
| **Szövegrajz (skálázható)** | `display.c:9-51` | `display_char/text/text_width/text_center`, integer `scale` (1,2,…) blokk-nagyítással | Hordozható; `font_glyph()`-re és `ssd1306_pixel/fill_rect`-re épül. |
| **Szám-formázók** | `display.c:55-82` | `u16_to_str`, `counter_dot` ("1234."), `mmss` ("MM:SS") | Tiszta C. (ESP-IDF-ben `snprintf` is jó, de ezek allokáció-mentesek.) |
| **Képernyő-flow minta** | `display.c:88-172`, `app.c` | `screen_main/run/error/warn/menu` + állapotgép görgetéssel, mező-szerkesztéssel, kiemelő kerettel | A célprojekt UI-jának nem 1:1 mása (más menüpontok), de a **render-minta** (clear→rajz→flush, dirty-flag, ~25 fps, képernyő-keret kiemelés) közvetlen sablon. |

### 1.2 Logika hordozható, I/O AVR-specifikus

| Asset | Fájl:sor | Hordozható rész | AVR-specifikus rész |
|---|---|---|---|
| **Enkóder kvadratúra** | `encoder.c:21-29` (`ttable`), `66-73` | Ben Buxton állapottábla — teljes detentnél ±1, pergést eldob | `ISR(PORTF_PORT_vect)` (`58`), `PORTF.IN`/`PIN0CTRL`/`INTFLAGS` regiszterek, `ATOMIC_BLOCK` |
| **Gomb debounce + short/long** | `button.c:21-82` | Idő-alapú debounce (`DEBOUNCE_MS=20`), short(≤2 s)/long(≥3 s) megkülönböztetés, `consume()` | `PORTA.IN & PIN6_bm` (`button.c:24`), `millis()` (timer.c) |

### 1.3 Tisztán AVR-specifikus (NEM átemelni)

| Asset | Fájl | Miért nem kell |
|---|---|---|
| `twi.c` | egész | `TWI0.MBAUD/MCTRLA/MSTATUS/MADDR/MDATA` regiszterek — ESP-IDF I2C driver váltja ki. |
| `ssd1306.c` `cmd()`+`flush()` I/O része | `13-19,45-50,63-82` | `twi_start/write/stop` hívások — átírandó ESP-IDF I2C-re. |
| `gpio.c` | egész | A tester kimenetei (relék, LED-ek, error-bemenet) — a SWD-programozó projekthez nem tartozik. |
| ISR-ek, `avr/io.h`, `avr/pgmspace.h`, `avr/interrupt.h` | több | Architektúra-fejlécek. |

---

## 2. Fontos hardver-részletek az AVR kódból

### 2.1 A panel: **SH1106**, nem SSD1306 (de SSD1306-kompatibilis init)

Bizonyíték `ssd1306.c:7-9`:
```c
/* Oszlop-eltolas: SSD1306 = 0, SH1106 (1.3" panelek) = 2.
   A panel SH1106: a 132 oszlopbol a lathato 128 oszlop 2-vel eltolva. */
#define SSD_COL_OFFSET 2
```
- Az SH1106 GDDRAM **132 oszlop széles**, a látható 128-as panel 2 oszloppal eltolva ül rajta → minden lap kiírásakor a kezdő oszlopcím **2** (`ssd1306.c:71-72`):
  ```c
  twi_write(0x00 | (SSD_COL_OFFSET & 0x0F));  /* col low nibble  = 0x02 */
  twi_write(0x10 | (SSD_COL_OFFSET >> 4));    /* col high nibble = 0x10 */
  ```
- **Ha az ESP-projekt valódi SSD1306 panelt használ → `COL_OFFSET = 0`.** Ezt tedd a kód tetejére konstansként/Kconfig-opcióként; a kód-logika mindkét panellal azonos.
- Az init `0x8D,0x14` (charge pump, `ssd1306.c:29`) az SSD1306 parancsa. SH1106 panelnél ez gyakran no-op vagy a DC-DC `0xAD,0x8B` lenne, de a gyakorlatban a legtöbb SH1106 modul ezzel a szekvenciával is bekapcsol (a panelnek saját töltőpumpa-engedélyezője van) — **ezért működik egységesen**.

### 2.2 Címzési mód: **page addressing** (lap-címzés) — robusztus flush

`ssd1306.c:30` és `63-83`:
```c
0x20, 0x02,   /* memory mode: page (lap-cimzes) */
```
A `flush()` lap-onként (`0xB0|page`, `ssd1306.c:70`) állítja be a lapot és az oszlop-kezdőt, majd 128 byte adat. **Ez panel-független** (SSD1306/SH1106 egyaránt), ezért érdemes megtartani a horizontal-addressing helyett.

### 2.3 Forgatás: **180°-ban fordított panel**

`ssd1306.c:31-32`:
```c
0xA0,   /* segment remap (180 fok forditas: fejjel-lefele panel) */
0xC0,   /* COM scan inc (180 fok forditas) */
```
- `0xA0` = segment remap **kikapcsolva** (col0→SEG0), `0xC0` = COM scan **increment**.
- A komment szerint a **panel fizikailag fejjel lefelé van szerelve**, ezért az „alap” (nem tükrözött) beállítás adja a helyes képet. **Normál állású panelnél a megszokott érték `0xA1` + `0xC8`.** Ezt a két byte-ot a szerelési orientációhoz kell igazítani a cél-HW-n — tedd konfigurálhatóvá.

### 2.4 Egyéb init paraméterek (idézve)

| Parancs | Byte(ok) | Jelentés (`ssd1306.c:24-39`) |
|---|---|---|
| Display off | `0xAE` | kikapcsolva init alatt |
| Clock div | `0xD5,0x80` | oszc. frekvencia/osztó, default |
| Multiplex | `0xA8,0x3F` | 1/64 → **64 sor** (128×64 panel) |
| Display offset | `0xD3,0x00` | nincs vertikális eltolás |
| Start line | `0x40` | RAM start line 0 |
| Charge pump | `0x8D,0x14` | belső töltőpumpa BE (3.3 V-ról) |
| Memory mode | `0x20,0x02` | **page addressing** |
| Seg remap | `0xA0` | lásd 2.3 (fordítás) |
| COM scan | `0xC0` | lásd 2.3 (fordítás) |
| COM pins | `0xDA,0x12` | alternatív COM, 128×64 elrendezés |
| **Kontraszt** | `0x81,0x7F` | **0x7F = 127 (közepes)**, 0..255 skálán |
| Precharge | `0xD9,0xF1` | precharge period |
| VCOM detect | `0xDB,0x40` | VCOMH deselect |
| Resume RAM | `0xA4` | RAM tartalom követése |
| Normal | `0xA6` | nem invertált |
| Display on | `0xAF` | bekapcsolás |

I2C cím (`ssd1306.c:5`): `0x78` = `0x3C << 1` (8-bites, write). **ESP-IDF-ben 7-bites cím kell → `0x3C`.** Ha a modul 0x3D, akkor `0x78`→`0x7A`, ESP-IDF-ben `0x3D`.

A power-on várakozás busy-loop (`ssd1306.c:42-43`) → ESP-IDF-ben `vTaskDelay(pdMS_TO_TICKS(50..100))`.

---

## 3. Port-terv: `components/display_oled/`

### 3.1 I2C driver csere — válassz API-t

A terv (`ESP32S3_SWD_PROG_Plan.md:144`) `driver/i2c`-t (legacy) említ, de új projekthez az **`driver/i2c_master`** (új API, ESP-IDF v5.2+) ajánlott — tisztább, és jól illik a „lap = egy tranzakció" flush-mintához. Mindkettő működik; alább az újat mutatom.

**Setup (init):**
```c
#include "driver/i2c_master.h"

#define OLED_ADDR        0x3C            // 7-bites! (AVR 0x78 >> 1)
#define OLED_COL_OFFSET  0              // SSD1306=0 ; SH1106 1.3"=2  (lásd 2.1)

static i2c_master_dev_handle_t s_dev;

void oled_i2c_init(void) {
    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = 8, .scl_io_num = 9,        // terv: SDA=GPIO8, SCL=GPIO9
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,    // külső felhúzó ajánlott!
    };
    i2c_master_bus_handle_t bushdl;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &bushdl));
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_ADDR,
        .scl_speed_hz = 400000,                  // 400 kHz (terv 15.1)
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bushdl, &dev, &s_dev));
}
```

### 3.2 AVR → ESP-IDF függvény-megfeleltetés

| AVR (`twi.c`/`ssd1306.c`) | ESP-IDF (`i2c_master`) | Megjegyzés |
|---|---|---|
| `twi_init()` | `oled_i2c_init()` (3.1) | bus + device handle létrehozás |
| `twi_start(0x78)` + `twi_write(0x00)` + `twi_write(c)` + `twi_stop()` (`ssd1306.c:13-19`) | `i2c_master_transmit(s_dev, (uint8_t[]){0x00, c}, 2, -1)` | egy parancs-byte: control 0x00 + parancs |
| init `seq[]` ciklus (`ssd1306.c:45-50`) | egy tömbbe: `0x00` + `seq[...]` → egyetlen `i2c_master_transmit(... , 1+sizeof(seq), -1)` | egyetlen tranzakció a teljes init |
| `flush()` lap-fej (`ssd1306.c:68-73`) | `i2c_master_transmit(s_dev, (uint8_t[]){0x00, 0xB0|page, 0x00|(off&0xF), 0x10|(off>>4)}, 4, -1)` | oszlop-offset = `OLED_COL_OFFSET` |
| `flush()` adat (`ssd1306.c:75-81`) | egy 129 bájtos buffer: `[0]=0x40`, utána a lap 128 byte-ja → **egy** `i2c_master_transmit(..., 129, -1)` | lapt 128 byte egyben — sokkal gyorsabb mint byte-onként |
| `cmd(0xAF/0xAE)` (`ssd1306.c:85-86`) | `oled_cmd(0xAF/0xAE)` wrapper a control-byte mintára | display on/off |

> **Optimalizálás:** az AVR `flush()` byte-onként ír (`twi_write` 8×128-szor). ESP-IDF-ben **lap-onként egy 129 bájtos transzfer** (control 0x40 + 128 adat) — 8 tranzakció/frame. Még jobb, ha a control-byte után az egész 1024 byte-ot egy transzferben küldöd (page addressing mellett laponként cím kell, így marad a 8 tranzakció — ez bőven jó).

### 3.3 Framebuffer flush stratégia

- A `fb[1024]` framebuffert **változatlanul** átemeled (`ssd1306.c:11`). Az ESP-S3-on bőven elfér belső RAM-ban (ne PSRAM, hogy gyors legyen).
- **Dirty-flag / részleges flush:** az AVR `app.c:254` már `dirty`-flaggel és ~25 fps cap-pel rajzol. Vidd át ezt a mintát az `ui_task`-ba (terv 15.1: „csak változásra/~15–30 fps").
- **Opcionális továbbfejlesztés:** lapt-szintű dirty bitmask (8 bit), és csak a megváltozott lapokat flush-old. Az AVR nem csinálja, de olcsó nyereség, ha a render-loop sűrű.
- **Thread-safety:** a flush az `ui_task`-ból fusson; a SWD-programozó progress-callback ne hívja közvetlenül az I2C-t, hanem `target_state`/queue-n keresztül jelezzen az `ui_task`-nak (terv 13.3, 16).

### 3.4 Melyik fájlt hogyan

| Cél fájl | Forrásból | Módosítás |
|---|---|---|
| `oled_font5x7.c/.h` | `font.c` + `font.h` | `PROGMEM`→`const`, `pgm_read_byte(&FONT[idx][i])`→`FONT[idx][i]`. Egyébként **változatlan**. |
| `oled_bigfont.c/.h` + `font_swis721.h` | `mikrofont.c/.h` + `font_swis721.h` | `font_swis721.h`: `PROGMEM`→`const`. `mikrofont.c`: `pgm_read_byte(p)`→`*p`, hívásnév `ssd1306_pixel`→`oled_pixel`. Logika **változatlan**. |
| `oled_gfx.c/.h` | `ssd1306.c` (`56-116`) + `display.c` (`9-82`) | A rajz-primitívek és szövegrajz **változatlan** logika; csak a fájlszervezés. |
| `oled_panel.c/.h` | `ssd1306.c` (init `seq[]`, `cmd`, `flush`, `on/off`) | I/O réteget átírni `i2c_master`-re (3.2). `seq[]` adat **változatlan**, csak a 2.1/2.3 konfig-konstansok kiemelése. |
| `oled_screens.c/.h` | `display.c` (`88-172`) — **referenciaként** | A célprojekt képernyői **mások** (fájllista, cél-detekció, progress bar — terv 15.2). Az AVR `screen_menu` görgetés+keret+mező-kiemelés **mintáját** vedd át, a tartalmat újraírd. |
| `twi.c` | — | **NE** emeld át. |

---

## 4. Port-terv: `components/input_enc/`

### 4.1 Enkóder — két út (a terv PCNT-et ajánl, 14.1)

**A) PCNT (ajánlott, terv 14.1):** az ESP32-S3 PCNT hardveres kvadratúra-dekódot + glitch-filtert ad, ISR-jitter nélkül. Ekkor az AVR `ttable` állapotgépre **nincs szükség** — a HW számol.
```c
#include "driver/pulse_cnt.h"
// A=GPIO10 (edge), B=GPIO11 (level) — 4x dekód két csatornával
pcnt_unit_config_t uc = { .high_limit = 1000, .low_limit = -1000 };
pcnt_new_unit(&uc, &unit);
pcnt_glitch_filter_config_t gf = { .max_glitch_ns = 1000 };   // ~1us glitch-szűrő
pcnt_unit_set_glitch_filter(unit, &gf);
// A csatorna él, B szint adja az irányt; a B-csatorna fordított konfigja a 4x:
pcnt_new_channel(unit, &(pcnt_chan_config_t){.edge_gpio_num=10,.level_gpio_num=11}, &chA);
pcnt_channel_set_edge_action(chA, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
pcnt_channel_set_level_action(chA, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
// (második csatorna A/B felcserélve a teljes 4x-hez)
```
- A `ui_task` periodikusan `pcnt_unit_get_count()`-tal olvas, deltát képez (ahogy az AVR `encoder_read()` `g_delta`-t olvas és nulláz, `encoder.c:48-56`), és **detent-enként** osztja le (a legtöbb mechanikus enkóder 4 él/detent → `delta/4`).
- A `+1/−1` tick → `xQueueSend` az esemény-queue-ra (terv 14.3).

**B) GPIO ISR + a meglévő `ttable` (ha PCNT-et máshová kell):** az AVR `encoder.c` állapotgépe **változatlanul** átemelhető. Csere:
- `ISR(PORTF_PORT_vect)` → `gpio_isr_handler` (`gpio_install_isr_service` + `gpio_isr_handler_add` GPIO10 és GPIO11-re, `GPIO_INTR_ANYEDGE`).
- `PORTF.IN & PIN1/PIN0` → `gpio_get_level(10)/gpio_get_level(11)`.
- `ATOMIC_BLOCK` → `portENTER_CRITICAL` vagy `xQueueSendFromISR` közvetlenül az ISR-ből (a `g_delta` helyett).
- A `ttable[7][4]` (`encoder.c:21-29`) **szó szerint** marad.

> Javaslat: **PCNT** az alap (terv szerint, és a `dedic_gpio`-s SWD mellett nullára viszi az enkóder-CPU-t); a `ttable`-t tartsd meg fallbacknek/dokumentációnak.

### 4.2 Gomb (SW = GPIO12) — short/long

Az AVR `button.c` poll-alapú debounce + short/long logikája **közvetlenül hordozható** egy timer/task-poll-ba (pl. 5 ms-onként az `ui_task`-ban vagy egy `esp_timer` callbackben):
- `PORTA.IN & PIN6_bm` (`button.c:24`) → `gpio_get_level(12)` (aktív alacsony, belső pull-up: `gpio_set_pull_mode(12, GPIO_PULLUP_ONLY)`).
- `millis()` → `esp_timer_get_time()/1000` vagy `xTaskGetTickCount()*portTICK_PERIOD_MS`.
- Konstansok `config.h`-ból: `DEBOUNCE_MS=20` (`button.c:5`), `SHORT_PRESS_MAX_MS=2000`, `LONG_PRESS_MENU_MS=3000` (`config.h:13-15`). Ezek átemelhetők (a célprojektben akár rövidebb long is elég, pl. 800 ms).
- Esemény: `btn_short()`/`btn_long()` (`button.c:55-63`) → `BTN_SHORT`/`BTN_LONG` a queue-ra.

> Alternatíva ISR-rel: GPIO12 `GPIO_INTR_ANYEDGE` + `esp_timer` a nyomáshosszhoz. De a poll-alapú AVR-logika egyszerűbb és bőven elég — ezt javaslom átemelni.

### 4.3 Esemény-queue modell (terv 14.3)

```c
typedef enum { ENC_CW, ENC_CCW, BTN_SHORT, BTN_LONG } enc_event_t;
static QueueHandle_t s_evq;            // xQueueCreate(16, sizeof(enc_event_t))
// forrás (PCNT delta vagy gomb-poll) -> xQueueSend(s_evq, &ev, 0)
// fogyasztó: ui_task -> xQueueReceive(s_evq, &ev, portMAX_DELAY)
```
- A bevitel teljesen leválik a renderelésről (terv 14.3): a `ui_task` `xQueueReceive`-vel várakozik, és a kapott eseményre futtatja az állapotgépet (az AVR `app_task` `menu_task` mintájára, `app.c:81-142`), majd `dirty`-re rajzol+flush-ol.
- Az AVR „ébresztő nyomás elnyelése" + képernyő-időkorlát logika (`app.c:151-184`, `config.h:17` `OLED_TIMEOUT_MS`) opcionálisan átvehető beégés-védelemnek.

---

## 5. Konkrét fájl-átemelési lista

### `components/display_oled/`

| Új fájl | Forrás (AVR) | Művelet |
|---|---|---|
| `font5x7.c` + `.h` | `font.c`, `font.h` | **Másold**, `PROGMEM`→`const`, `pgm_read_byte`→közvetlen index. |
| `font_swis721.h` | `font_swis721.h` | **Másold**, `PROGMEM`→`const`. Adat változatlan. |
| `bigfont.c` + `.h` | `mikrofont.c`, `mikrofont.h` | **Másold**, `pgm_read_byte(p)`→`*p`, `ssd1306_pixel`→`oled_pixel`. |
| `oled_gfx.c` + `.h` | `ssd1306.c:56-116` + `display.c:9-82` | **Másold** a primitíveket és szövegrajzot; átnevezés `ssd1306_*`→`oled_*`. |
| `oled_panel.c` + `.h` | `ssd1306.c:5-86` (init/cmd/flush/on/off) | **Átírd** az I/O-t `i2c_master`-re (3.2); `seq[]` és a flush-szerkezet marad. `OLED_ADDR=0x3C`, `OLED_COL_OFFSET` konfig. |
| `oled_screens.c` + `.h` | `display.c:88-172` | **Referenciaként**: a render-mintát (clear→rajz→flush, keret-kiemelés, görgetés) vedd át, a tartalom a terv 15.2 képernyőihez új. |
| *(kihagyva)* | `twi.c`, `twi.h` | Nem kell. |

### `components/input_enc/`

| Új fájl | Forrás (AVR) | Művelet |
|---|---|---|
| `encoder.c` + `.h` | `encoder.c`, `encoder.h` | **Elsődleges: PCNT-alapú újraírás** (4.1/A). A `ttable` (`encoder.c:21-29`) megtartható kommentként/fallback GPIO-ISR-hez (4.1/B). |
| `button.c` + `.h` | `button.c`, `button.h` | **Másold** a debounce + short/long logikát; `PORTA.IN`→`gpio_get_level(12)`, `millis()`→`esp_timer_get_time()/1000`. Konstansok `config.h:13-15`-ből. |
| `input_enc.c` + `.h` | (új) | Esemény-queue (`enc_event_t`), `xQueueSend` a forrásokból, public `input_enc_get_event()`/queue-handle. |
| *(átveendő konstansok)* | `config.h:13-17` | `DEBOUNCE_MS`, `SHORT_PRESS_MAX_MS`, `LONG_PRESS_MENU_MS`, opc. `OLED_TIMEOUT_MS` → Kconfig vagy fejléc. |

---

## Összefoglaló (a legfontosabb megállapítások)

1. **A panel SH1106** (1.3"), nem SSD1306: `ssd1306.c:7-9` szerint **2 oszlop offset** kell (`SSD_COL_OFFSET 2`). Ha a cél-HW valódi SSD1306, ezt **0-ra** állítsd — egy konstans, a logika közös.
2. **Forgatás: a panel 180°-ban fordítva van szerelve** → `0xA0` (seg remap off) + `0xC0` (COM inc) az init-ben (`ssd1306.c:31-32`). Normál állású panelhez `0xA1`+`0xC8` kell — tedd konfigurálhatóvá.
3. **Kontraszt** közepes (`0x81,0x7F` = 127), **64 sor** (`0xA8,0x3F`), **page addressing** (`0x20,0x02`) — a lap-címzéses flush panel-független és átemelendő.
4. **Két font átemelhető szinte változatlanul:** az 5x7 ASCII (`font.c`) a menü/feliratokhoz, és a **Swis721 19x24 nagy szám-font** (`font_swis721.h`, csak `.`,`/`,0–9,`:`) a számláló- és MM:SS-kijelzéshez. Csak a `PROGMEM`/`pgm_read_byte` cseréje kell.
5. **Framebuffer + rajz-primitívek + szövegrajz** (`ssd1306.c:56-116`, `display.c:9-82`) teljesen platformfüggetlen, közvetlenül vihető. A flush byte-onkénti írása helyett ESP-IDF-ben **lap = 1 tranzakció** (control 0x40 + 128 byte) — 8 transzfer/frame.
6. **I2C:** `twi.c` eldobandó; AVR `twi_start/write/stop` → `i2c_master_transmit` az új `driver/i2c_master`-rel (SDA=8, SCL=9, 400 kHz, cím **0x3C** a 7-bites formában — az AVR 0x78 fele).
7. **Enkóder:** a terv szerint **PCNT** (hardveres kvadratúra + glitch-filter, ISR-jitter nélkül) az ajánlott; az AVR Ben-Buxton `ttable` (`encoder.c:21-29`) fallbacknek megtartható GPIO-ISR-rel. **Gomb** debounce + short/long (`button.c`) szinte változatlanul portolható poll-loopba (`PORTA.IN`→`gpio_get_level(12)`, `millis()`→`esp_timer`).
8. **Esemény-modell:** `enc_event_t {ENC_CW, ENC_CCW, BTN_SHORT, BTN_LONG}` → FreeRTOS queue → `ui_task`; a bevitel leválasztva a renderről, ahogy a terv 14.3 előírja és az AVR `app.c` dirty-flag + ~25 fps render-mintája mutatja.
