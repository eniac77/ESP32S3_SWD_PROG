# ILI9488 480×320 SPI + GT911 touch + LVGL — port-terv (SSD1306 leváltása)

**Cél:** a jelenlegi SSD1306/SH1106 128×64 I2C OLED (`components/display_oled` + `components/ui`)
leváltása **ILI9488 480×320 SPI** kijelzőre **GT911 kapacitív touch**-csal, **LVGL** UI-keretrendszerrel.

**Döntések (jóváhagyva):**
- **Enkóder + touch EGYÜTT** — az `input_enc` változatlan marad, LVGL encoder-indevként bekötve;
  a touch kiegészítés. Minimális regresszió, a projekt enkóder-centrikus marad.
- **LVGL** UI-keretrendszer (a hivatalos `esp_lvgl_port` integrációval).

> Ez a dokumentum **csak terv** — kódot nem ír. A SWD/FLM mag, `target_db`, `prog_session`,
> storage, `web_ui`, `net_wifi`, `ftp_srv`, `target_serial` **nem érintett**.

---

## 0. Hatáskör — mi változik, mi nem

| Réteg | Állapot | Megjegyzés |
|---|---|---|
| `components/display_oled/` | **Lecserélve** → `components/display_lcd/` | esp_lcd SPI panel (ILI9488) + GT911. A framebuffer/font-primitívek (`display_oled_pixel/text/big_*`) **megszűnnek** — LVGL widgetek váltják. |
| `components/ui/ui.c` (~760 sor) | **Újraírva** LVGL-re | A **képernyő-flow / állapotgép logika újrahasznosul**, a rajzolás 100%-ban cserélődik. |
| `components/input_enc/` | **Változatlan** | A meglévő `enc_event_t` queue → LVGL encoder-indev (4.3). |
| `main/main.c` | Init csere | `display_oled_init()` → `display_lcd_init()` (LVGL+esp_lcd+GT911). Init-sorrend marad. |
| SWD-mag, storage, web, wifi, ftp, serial | **Nincs hatás** | |

---

## 1. Lábkiosztás — szűkös, de befér (KRITIKUS)

A jelenlegi OLED 2 lábat foglal (SDA=GPIO8, SCL=GPIO9) — ezek **felszabadulnak**.
A SWD/UART/enkóder/AVR-ISP/PSRAM/flash/strapping foglaltság után **szabad GPIO-k:**

```
2, 8, 9 (OLED-ből felszabadul), 38, 39, 40, 41, 42, 47, 48   → 10 db
```

(Foglalt és kerülendő: SWD 4/5/6, UART 17/18, enkóder 10/11/12, AVR-ISP 7/15/16/21,
Vref 1, EN 13, LED 14, USB-native 19/20, PSRAM/flash 26–37, strapping 0/3/45/46.)

### Javasolt kiosztás (SPI2/FSPI host)

| Funkció | GPIO | Megjegyzés |
|---|---|---|
| **LCD SCLK** | 9 | SPI clock (volt OLED SCL) |
| **LCD MOSI** | 8 | SPI data (volt OLED SDA). MISO **nem kell** (write-only) |
| **LCD CS** | 38 | chip select |
| **LCD DC** | 39 | data/command |
| **LCD RST** | 40 | panel reset |
| **LCD BL** | 41 | háttérvilágítás — *opcionális*, fixre (3V3 + ellenállás) köthető → GPIO41 felszabadul tartaléknak |
| **Touch SDA** | 47 | GT911 I2C (új I2C busz, `I2C_NUM_0`) |
| **Touch SCL** | 48 | GT911 I2C |
| **Touch INT** | 42 | GT911 — a reset-alatti szintje választja az I2C-címet (0x5D/0x14) |
| **Touch RST** | 2 | GT911 reset (kell az cím-szekvenciához; nem strapping az S3-on) |

**Slack:** ha a BL fix 3V3-on van → **GPIO41 marad tartaléknak**. Egyébként nulla tartalék — bővítés
(pl. második periféria) később lábkiosztás-átgondolást igényel.

**Megjegyzések:**
- SPI2 (FSPI) szabad: a PSRAM/flash a SPI0/1-en megy.
- A GT911 INT/RST időzítését (cím-választás) az `esp_lcd_touch_gt911` kezeli — csak külön láb kell.
- ILI9488 SPI-hez ajánlott külső szintillesztés/tápszűrés a HW-n; a 480×320 panel sávszélessége miatt
  a SPI-vezetékek legyenek rövidek (40–80 MHz).

---

## 2. Komponens-struktúra + managed függőségek

Új komponens `components/display_lcd/` a régi `display_oled` helyén. A projekt mintája szerint
(mint `storage_lfs` → littlefs, `net_wifi` → mdns) a managed deps a komponens saját
`idf_component.yml`-jében:

```yaml
# components/display_lcd/idf_component.yml
dependencies:
  lvgl/lvgl: "^9.2.0"                      # vagy ^8.4 ha v8-at akarunk
  espressif/esp_lvgl_port: "^2.4.0"        # LVGL <-> esp_lcd + touch + encoder integráció
  atanisoft/esp_lcd_ili9488: "^1.1.0"      # ILI9488 panel (RGB666 konverzióval!) — FIGYELEM: atanisoft/, NEM espressif/
  espressif/esp_lcd_touch_gt911: "^1.1.0"  # GT911 kapacitív touch
```

> **D0/D1/D2 javítás (feloldott verziók):** az ILI9488 panel-driver a Component
> Registry-ben **`atanisoft/esp_lcd_ili9488`** néven van (a fenti `espressif/...`
> tévedés volt). A ténylegesen feloldott verziók: **lvgl 9.5 (v9 API!)**,
> **esp_lvgl_port 2.8**, **atanisoft/esp_lcd_ili9488 1.1.1**,
> **espressif/esp_lcd_touch_gt911 1.2.0** (+ esp_lcd_touch). A CMake
> `REQUIRES`-ben az ILI9488 neve namespace nélkül: `esp_lcd_ili9488`.

**Miért `esp_lvgl_port`:** a hivatalos komponens egyben kezeli az LVGL tick-et, a render-taszkot,
az esp_lcd flush-callbacket, a touch- és **encoder-indevet** — drasztikusan kevesebb glue-kód, és a
thread-safety (lock) is benne van (`lvgl_port_lock/unlock`).

**Flash-költség:** LVGL + fontok ~150–400 KB. A `factory` app **3 MB** (`partitions.csv`) → bőven elfér,
de build után érdemes a `idf.py size`-t ránézni (a jelenlegi app + LVGL).

---

## 3. Init-váz (`display_lcd_init`)

Logikai lépések (kód-skeleton, nem teljes):

```c
// 1) SPI busz (FSPI) — MOSI=8, SCLK=9, max_transfer ~ a draw-buffer mérete
spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);

// 2) esp_lcd panel IO (SPI) — CS=38, DC=39, pclk 40–80 MHz
esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io);

// 3) ILI9488 panel — RST=40, bits_per_pixel=18 (RGB666!), a komponens konvertál
esp_lcd_new_panel_ili9488(io, &panel_cfg, DRAW_BUF_PX, &panel);
esp_lcd_panel_reset(panel); esp_lcd_panel_init(panel);
esp_lcd_panel_invert_color/​mirror/​swap_xy(...);   // orientáció finomhangolás

// 4) GT911 touch — saját I2C bus (SDA=47, SCL=48), INT=42, RST=2
i2c_new_master_bus(...); esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &tp);

// 5) esp_lvgl_port — egy taszk, lock, tick
lvgl_port_init(&lvgl_cfg);
lv_disp = lvgl_port_add_disp(&disp_cfg);          // draw-buffer PSRAM/internal
lvgl_port_add_touch(&touch_cfg);                  // GT911 -> LVGL pointer indev
// 6) Enkóder indev (4.3)
lv_indev_encoder = lvgl_port_add_encoder(&enc_cfg);  // input_enc queue -> LVGL encoder
```

**Draw-buffer:** LVGL nem kell teljes framebuffer; partial buffer elég. Javaslat: 480×320 / 10 sor ×
2 puffer, **internal DMA-RAM**-ban (gyors), vagy nagyobb buffer PSRAM-ban (S3 tud PSRAM→SPI DMA-t).
Teljes RGB565 framebuffer = 480×320×2 = **300 KB** PSRAM-ban opcionálisan elfér, ha teljes-frissítést akarunk.

---

## 4. UI újraírás — `ui.c` → LVGL képernyők

A jelenlegi képernyő-flow **megmarad**, csak a rajzolás lesz LVGL-widget. Az állapotgép
(`SCR_IDLE/MENU/FWLIST/FWSEL/AVRLIST/AVRSEL/PLACEHOLDER`) és a navigáció (`list_move`,
`enter_screen`, `ui_handle`) logikája **újrahasznosul**.

### 4.1 Képernyő-leképezés

| Jelenlegi (OLED, kézi rajz) | LVGL megfelelő |
|---|---|
| `draw_idle()` — 3 állapotsor | `lv_label`-ek egy konténerben (WiFi/Target/Serial); a `target_state`-ből frissítve |
| `ui_draw_list()` — görgetett lista, keretes kijelölés | **`lv_list`** vagy `lv_roller`; a kijelölés a LVGL fókusz/`lv_group`. A scroll automatikus. |
| `MENU_ITEMS` főmenü | `lv_list` gombokkal (Program fw / Cel info / AVR ISP / …) |
| `SCR_FWLIST` / `SCR_AVRLIST` fájllista | `lv_list` a `storage_src_list` elemeiből |
| `draw_fwsel()` / `draw_avrsel()` megerősítő | **`lv_msgbox`** ("Selected: …  OK=flash") |
| `ui_flash_cb` / `ui_avr_flash_cb` progress-bar | **`lv_bar`** + fázis-`lv_label` + százalék-`lv_label` |
| detect/eredmény "Nyomj gombot" | `lv_msgbox` OK gombbal |
| `ui_draw_header()` inverz fejléc | `lv_obj` top-bar style, vagy `lv_label` + háttér |

**Előny:** a fix 10-karakteres csonkolás (`UI_LIST_MAXCH`), a kézi sor-koordináták (`UI_ROW_Y`),
a scale-trükkök **mind eltűnnek** — 480×320-on bőséges hely, igazi fontok, gördülő listák.

### 4.2 Bevitel-integráció

- **Enkóder:** az `input_enc` queue (`ENC_CW/CCW/BTN_SHORT`) → LVGL **encoder-indev** read-cb.
  LVGL `lv_group`-ba veszi a fókuszálható widgeteket; forgatás = fókusz-léptetés, `BTN_SHORT` =
  aktiválás. `BTN_LONG` → "vissza" navigáció (saját kezelés, mert LVGL encodernek nincs natív "back").
- **Touch:** GT911 → LVGL pointer-indev (az `esp_lvgl_port` automatikusan). A listák/gombok kattinthatók.
- A két indev **párhuzamosan** él; ugyanazokat a widgeteket vezérlik.

---

## 5. Flash-alatti render — SWD-jitter elkerülése (KRITIKUS)

A mérföldkő tanulsága: flash/detect alatt a **WiFi-rádió le van állítva** (RF-zaj glitch-eli a
bit-bang SWD-t), és az **OLED-progress-throttle (250 ms)** volt a legnagyobb rejtett lassító.
Az SPI-kijelzőnél ezt át kell vinni:

1. **Külön periféria:** az ILI9488 SPI2+DMA-n megy, a SWD `dedic_gpio`-n — közvetlen ütközés nincs.
2. **PSRAM busz-kontenció:** a PSRAM↔SPI DMA jitter-t adhat a PSRAM-rezidens SWD-kódhoz/adathoz.
   Mérséklés: a draw-buffer **internal DMA-RAM**-ban (ne PSRAM-ban) flash közben; vagy a render-
   frekvenciát fogd vissza.
3. **Szálbiztos progress:** a `prog_session` callback **ne** rajzoljon közvetlenül (LVGL nem
   thread-safe a saját taszkján kívül). A flash-szálból csak **jelezz** (flag/`lvgl_port_lock` +
   `lv_bar_set_value` + unlock, vagy `lv_async_call`), a render az LVGL-taszkban történjen.
4. **Throttle marad:** a progress-frissítés ~4–7 fps (a mostani 250 ms elv), hogy a render ne
   lopjon időt a flash-től.
5. **Opció:** a legkritikusabb SWD bit-bang szakaszokra a kijelző-frissítés is **szüneteltethető**
   (ahogy a WiFi), ha HW-méréskor jitter mutatkozik.

> A jelenlegi szinkron modell (`ui_start_flash` blokkol, cb rajzol) átalakul:
> a flash külön történik, a UI a megosztott progress-állapotot rendereli.

---

## 6. Memória / particionálás

- **PSRAM kész** (octal 8 MB, `SPIRAM_USE_MALLOC=y`) — LVGL heap + draw-buffer + opc. teljes FB elfér.
- **Partíció:** nem kell változtatni; a `factory` 3 MB elég az LVGL-lel. Build után `idf.py size` ellenőrzés.
- **LVGL config** (`lv_conf.h` / Kconfig): színmélység 16-bit (RGB565, az ili9488 komponens konvertál
  RGB666-ra), `LV_MEM` PSRAM-ba irányítva, csak a szükséges fontok/widgetek befordítva (flash-takarékos).

---

## 7. Kritikus buktató: ILI9488 SPI = RGB666

Az ILI9488 **SPI-módban nem támogat 16 bit/pixelt (RGB565)**, csak 18 bitet (RGB666, 3 bájt/pixel).
→ ~50%-kal több SPI-forgalom egy ILI9341-hez képest. Az **`esp_lcd_ili9488` komponens menet közben
RGB565→RGB666 konvertál** — ezért ezt használjuk, **nem** írunk saját panelt. A `panel_cfg`-ben a
`bits_per_pixel = 18`, az LVGL belül 16-bit marad.

SPI @ 40–80 MHz: teljes 480×320×3 ≈ 450 KB → ~90 ms (40 MHz) / ~45 ms (80 MHz). UI-hoz a **partial
refresh** bőven elég; teljes-frissítés csak ritkán (képernyőváltás).

---

## 8. Kockázatok + HW-validációs checklist

| Kockázat | Mérséklés / ellenőrzés |
|---|---|
| ILI9488 RGB666 / rossz színek | `esp_lcd_ili9488` komponens; `invert_color` próbálgatás HW-n |
| Orientáció (480×320 fekvő/álló) | `swap_xy`/`mirror` flag-ek HW-n beállítva |
| GT911 I2C-cím (0x5D/0x14) | INT/RST szekvencia a komponensben; logikai analizátor ha nem jön touch |
| **SWD-jitter flash közben** | flash + verify valódi STM32-n (F030/F407), idő-regresszió mérése vs. ~3,1 s |
| PSRAM↔SPI DMA kontenció | draw-buffer internal RAM-ban flash közben; mérés |
| Flash-méret túlcsordulás | `idf.py size` a 3 MB `factory`-ra |
| Lábkiosztás-konfliktus | a 10 szabad GPIO pontos bekötése, BL fix → 41 slack |

---

## 9. Munkafázisok (javasolt sorrend)

1. **D0 — Komponens-váz:** `display_lcd` komponens + `idf_component.yml` (lvgl, lvgl_port, ili9488,
   gt911), üres `display_lcd_init`, zöld build (managed deps letöltése).
2. **D1 — Panel bring-up:** SPI + ILI9488 init, „Hello" teszt-kép HW-n (szín/orientáció).
3. **D2 — Touch + indev:** GT911 + LVGL pointer-indev; enkóder encoder-indev (input_enc bekötés).
4. **D3 — UI-port:** `ui.c` képernyők LVGL-re (idle → menü → fw-lista → fwsel → progress → detect →
   AVR-flow). A state-flow átemelése.
5. **D4 — Flash-progress:** szálbiztos progress-modell (5. szekció), throttle, és valódi flash-teszt
   STM32-n — **idő-regresszió mérése** (vs. ~3,1 s F030).
6. **D5 — Cleanup:** `display_oled` eltávolítása, `main.c` init-csere, dokumentáció (CLAUDE.md,
   plan 15. szekció frissítése), zöld build + commit.

---

## Összefoglaló (legfontosabb megállapítások)

1. **Megvalósítható**, jól támogatott (esp_lvgl_port + esp_lcd_ili9488 + esp_lcd_touch_gt911).
   A fő munka a **`ui.c` LVGL-re írása**; a SWD-mag és a platform-rétegek érintetlenek.
2. **Lábkiosztás befér, de szűkös** (10 szabad GPIO, BL-fixelve 1 slack). Pontos bekötés az 1. szekcióban.
3. **ILI9488 SPI = RGB666** (nem RGB565) — a komponens konvertál; ne írj saját panelt.
4. **SWD-időzítés a fő kockázat:** szálbiztos, throttle-olt progress + internal draw-buffer flash közben;
   HW-n mérni az idő-regressziót a ~3,1 s F030-hoz képest.
5. **Enkóder megmarad** LVGL encoder-indevként, a touch kiegészítés — minimális regresszió.
</content>
</invoke>
