# CLAUDE.md

Ez a fájl a Claude Code (és más AI-asszisztensek) számára ad iránymutatást a repó-ban való munkához.

## Projekt

**ESP32-S3 önálló SWD programozó + konfigurátor** — PC nélküli készülék, amely:

1. **LittleFS-ből** firmware-t (`.bin`) olvas és **SWD**-n (SWCLK/SWDIO) felprogramozza a csatlakoztatott **STM32** célt (családok: F0, F1, F3, F4, F7, L0, L1, L4, G0). A flash-kód **CMSIS `.FLM`** blobokból fut a cél RAM-jában (RAM flash loader).
2. **soros vonalon** (UART, sima 3.3 V) kommunikál a cél STM32 *futó alkalmazásával*: bináris **`.cfg`** fájlok fel-/letöltése, élő adat olvasása.
3. helyileg kezelhető **gombos enkóder + SSD1306 OLED**-ről (fájlválasztás, cél-típus, %-os állapot).
4. WiFi-n **webszerver UI**-t (REST + WebSocket) és **FTP-szervert** ad a LittleFS eléréséhez.

**Modul:** ESP32-S3-**N16R8** (16 MB flash, 8 MB **octal** PSRAM). **Platform:** ESP-IDF.

> A két célinterfész fizikailag külön van: **SWD** = firmware-flashelés (mag megállítva); **UART** = konfig + élő adat a *futó* célalkalmazással. A session-logika tiltsa a flash + config egyidejűséget ugyanazon a célon.

## Állapot

Jelenleg **csak a terv létezik** (`reference/ESP32S3_SWD_PROG_Plan.md`), kód még nincs. A teljes specifikáció, lábkiosztás, fázisos ütemterv és architektúra a tervben van — **bármilyen implementáció előtt olvasd el**.

## Architektúra elve (KRITIKUS, ne sértsd meg)

A **SWD/FLM mag platformfüggetlen**: kizárólag SWD-regisztertranzakció. Az egyetlen platformspecifikus rész a 4-függvényes PHY HAL:

```c
void     swd_phy_seq_out(uint32_t bits, int n);  // n bit LSB-first
uint32_t swd_phy_seq_in(int n);
void     swd_phy_dir(bool drive);                // turnaround
void     swd_phy_idle(int clocks);               // idle/reset SWDIO=1
```

Rétegek (alulról fölfelé): `swd_phy` (dedic_gpio HAL) → `adiv5` (DP/AP transport) → `cortexm_debug` (halt/reset, mem R/W) → `flm_runner` (FLM futtatás) → `prog_session` (orchestráció). A magban ne legyen platform- vagy család-specifikus elágazás — az a `target_db`-be és a HAL-ba tartozik.

## Tervezett komponens-struktúra (ESP-IDF)

```
components/
  swd_phy/        # dedic_gpio HAL (platformspecifikus)
  adiv5/          # DP/AP transport, switch, ACK/parity
  cortexm_debug/  # halt/reset, mem R/W, DCRSR/DCRDR
  flm_runner/     # call_function + FLM futtatás
  flm_blobs/      # generált C tömbök (PrgCode/PrgData/DevDsc)
  target_db/      # DEV_ID → FLM + flash-size reg
  prog_session/   # SWD programozás orchestráció
  target_serial/  # UART híd: .cfg fel/le, élő adat
  target_state/   # közös élő-adat modell (UI + WS forrása)
  storage_lfs/    # LittleFS mount + fájl-API + lock
  input_enc/      # enkóder ISR/PCNT + gomb → eseménysor
  display_oled/   # SSD1306 driver + UI képernyők
  net_wifi/       # STA/AP, provisioning, mDNS
  web_ui/         # esp_http_server: REST + WebSocket
  ftp_srv/        # FTP a LittleFS fölött
tools/flm_extract.py   # build-time .FLM → C tömb (pyelftools)
flm_packs/             # vendored .FLM források (ST DFP)
```

## Hardver — N16R8 lábkiosztás (FONTOS megkötések)

**Fenntartott, NE oszd ki:**
- Octal PSRAM (R8): **GPIO33–37 foglalt**, sose oszd ki perifériára.
- Belső flash SPI: GPIO26–32 foglalt.
- Strapping: GPIO0, 3, 45, 46 — kerüld.
- GPIO19/20: native USB pad — fenntartva a jövőbeli USB hosthoz.

**Szabadon kiosztható:** GPIO1–18, 21, 38–42, 47, 48.

**Javasolt kiosztás:** SWCLK=GPIO4, SWDIO=GPIO5, nRST=GPIO6 (OD+pullup), UART TX=GPIO17, UART RX=GPIO18, OLED SDA=GPIO8, OLED SCL=GPIO9, Enkóder A=GPIO10, B=GPIO11, SW=GPIO12, Vref ADC=GPIO1, táp EN=GPIO13, LED=GPIO14.

## Kulcs sdkconfig (N16R8)

```
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y          # octal PSRAM (R8) — kötelező R8-hoz
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_PARTITION_TABLE_CUSTOM=y   # partitions.csv
```

Particionálás: 3 MB `factory` app (0x10000), ~13 MB `storage` LittleFS (0x310000). Lásd a terv 3. szekcióját.

## Build és parancsok (ESP-IDF v5.5.1 — működik, ellenőrizve)

Az ESP-IDF itt van: `C:\Users\jenei\esp\v5.5.1\esp-idf`. **Minden shellben aktiválni kell** (a környezet nem perzisztens a PowerShell-hívások között), és az export + idf.py hívásnak egy parancsban kell lennie. Build (PowerShell):

```powershell
. "C:\Users\jenei\esp\v5.5.1\esp-idf\export.ps1"; idf.py build
```

Egyéb:
```powershell
idf.py set-target esp32s3        # csak elsőre / target váltáskor
idf.py -p <PORT> flash monitor
idf.py add-dependency "joltwallet/littlefs"
```

**Build-környezeti buktatók (megtapasztalt):**
- Az IDF tools eredetileg csak riscv targetre volt telepítve → az `export.ps1` nem tette az xtensa fordítót a PATH-ra ("Did not find file Compiler/-ASM"). Megoldás: egyszer lefuttatva `install.ps1 esp32s3`, utána jó.
- `partitions.csv`: **ne legyen sor végi `# komment`** az adatsorok után — a 6. oszlopot flag-nek veszi. A komment külön sorba kerüljön.
- `nincs gh` telepítve; remote: `origin = https://github.com/eniac77/ESP32S3_SWD_PROG.git`. Commit + push **csak zöld build után**.

## Fontos technikai részletek (a tervből)

- **Connect-under-reset KÖTELEZŐ:** nRST assert → SWD bring-up → DEMCR `VC_CORERESET` → nRST release → halt a reset-vektoron. Enélkül alvó/lockolt/WDG-s cél megfekszik.
- **FLM ABI** (FlashOS.h): `Init/UnInit/EraseSector/EraseChip/ProgramPage/Verify`, mind **0 = siker**.
- **FLM-hívás xPSR = 0x01000000** (T-bit!) — enélkül INVSTATE fault. PC/LR Thumb-bittel (`|1`).
- **SWD bring-up:** ≥50 SWCLK (SWDIO=1) → JTAG-to-SWD switch `0xE79E` LSB-first → ≥50 SWCLK → DPIDR.
- **SWD sebesség:** kezdj 200–500 kHz-en, bring-up után told fel. Az SWD szinkron → clock-stretch mindig OK, WiFi/FreeRTOS jitter nem töri el.
- **Kis RAM-ú cél** (F030F4, L011): FLM+stack+buffer beférjen; a legkisebb tagra méretezett FLM-et válaszd.
- **RDP:** RDP1 → csak mass-erase után írható; RDP2 végleges.
- **LittleFS egyidejűség:** web + FTP + prog **közös mutex** mögött; ne írj egy fájlt két helyről. Minden fájlművelet a `storage_lfs` API + lock alatt.

## Munkavégzési elvek

- **A nyelv magyar** — a terv, kommentek és kommunikáció magyarul. Tartsd a magyar terminológiát kód-kommentekben is, ha a környező kód ezt követi.
- A terv a forrás: ha valami ütközik vele, jelezd, mielőtt eltérnél.
- Fázisos ütemterv (terv 17. szekció): **A-track** = SWD/FLM mag (kritikus út, A0–A5), **B-track** = platform (LittleFS, OLED/enkóder, WiFi, web, FTP, serial), **C** = integráció. Javasolt kezdés: A0–A2 + B0/B1 párhuzamosan. Az A0-t F411 célon érdemes kezdeni (nagy RAM, jól dokumentált).

## Referenciák a maghoz

- **pyOCD** / **probe-rs**: FLM-betöltés + `call_function` + CMSIS-pack parse minta.
- **Black Magic Probe** (`target/`): család-driverek + ADIv5 (GPLv3 — termékhez figyelni a licencre).
- **FlashOS.h** (ARM CMSIS-Pack): az FLM ABI definíciója.
- **u8g2**: SSD1306 + fontok + menü (ha nem saját lib).

Teljes részletek: [reference/ESP32S3_SWD_PROG_Plan.md](reference/ESP32S3_SWD_PROG_Plan.md).

**Kijelző/enkóder port (B1-hez):** [reference/NX80_display_port.md](reference/NX80_display_port.md) — az NX80TESTER_AVR projektből átemelhető SSD1306/SH1106 driver, fontok (5×7 ASCII + Swis721 19×24 szám-font), enkóder/gomb logika, AVR→ESP-IDF megfeleltetéssel. Fontos: a referencia panel **SH1106**, 2 oszlop offset, 180° fordítva.
