# CLAUDE.md

Ez a fájl a Claude Code (és más AI-asszisztensek) számára ad iránymutatást a repó-ban való munkához.

## Projekt

**ESP32-S3 önálló SWD programozó + konfigurátor** — PC nélküli készülék, amely:

1. **LittleFS-ből** firmware-t (`.bin`) olvas és **SWD**-n (SWCLK/SWDIO) felprogramozza a csatlakoztatott **STM32** célt (családok: F0, F1, F2, F3, F4, F7, H7, H5, G0, G4, L0, L1, L4, L5, U0, U5, WB, WL, WBA, C0 — **80 DEV_ID** a teljes CubeProgrammer FlashLoader palettához igazítva). A flash-kód **CMSIS `.FLM`** blobokból fut a cél RAM-jában (RAM flash loader).
2. **soros vonalon** (UART, sima 3.3 V) kommunikál a cél STM32 *futó alkalmazásával*: bináris **`.cfg`** fájlok fel-/letöltése, élő adat olvasása.
3. helyileg kezelhető **gombos enkóder + ILI9488 480×320 SPI TFT + GT911 kapacitív touch (LVGL)**-ről (fájlválasztás, cél-típus, %-os állapot). *(Az SSD1306 OLED-verzió a `main` ágon él tovább; ez a `feat/ili9488-lvgl` branch a TFT/touch változat.)*
4. WiFi-n **webszerver UI**-t (REST + WebSocket) és **FTP-szervert** ad a LittleFS eléréséhez.

**Modul:** ESP32-S3-**N16R8** (16 MB flash, 8 MB **octal** PSRAM). **Platform:** ESP-IDF.

> A két célinterfész fizikailag külön van: **SWD** = firmware-flashelés (mag megállítva); **UART** = konfig + élő adat a *futó* célalkalmazással. A session-logika tiltsa a flash + config egyidejűséget ugyanazon a célon.

## Állapot

🎯 **MÉRFÖLDKŐ (2026-06-29): működő SWD-flash valódi hardveren.** Az eszköz felprogramozott és **ellenőrzött** egy valódi STM32F030x8-at (DEV_ID 0x440), 29 KB-ot `0x08000000`-ra, nRST nélkül (SYSRESETREQ), enkóderről indítva, **~3,1 s** alatt, megbízhatóan. A teljes történet (bring-up hibalánc + javítások, a glitch gyökéroka = **WiFi rádió zaja**, és a 214 s → 3,1 s sebesség-optimalizálás, ~70×): **[reference/MILESTONE_SWD_HW.md](reference/MILESTONE_SWD_HW.md) — olvasd el SWD-munka előtt.**

A **teljes szoftveres váz kész és zöldre fordul** (esp32s3). Mind a 16 komponens implementálva, az end-to-end flash-út be van kötve **és HW-n igazolt (F030)**. A teljes spec/lábkiosztás/ütemterv a tervben: `reference/ESP32S3_SWD_PROG_Plan.md` — **implementáció előtt olvasd el**.

🖥️ **Kijelző-váltás (ezen a `feat/ili9488-lvgl` branchen):** az SSD1306 OLED helyett **ILI9488 480×320 SPI TFT + GT911 touch + LVGL v9** (`display_lcd` komponens). A szoftveres port **kész és zöldre fordul** (D0–D3, commit `ea6888d`); a `display_oled` komponenst eltávolítottuk (D5). Hátra a **HW bring-up** (D4): valódi panel bekötése, orientáció/szín/SPI-frekvencia/GT911 hangolás. Port-terv: `reference/ILI9488_LVGL_port.md`. *(Az OLED-verzió a `main` ágon él tovább.)*

**SWD-munka kritikus tudnivalók (HW-n megtanulva, részletek a mérföldkő-doksiban):**
- A flash/detect idejére a **WiFi rádiót le KELL állítani** (`net_wifi_radio_pause`) — különben a rádió zaja glitch-eli a bit-bang SWD-t (re-sync 292 vs 2).
- SWDIO turnaround tri-state: `GPIO.func_out_sel_cfg[SWDIO].oen_sel=1` kell (dedic_gpio + GPIO_ENABLE).
- A read-mintavétel a **low fázisban** van; az írás után **trailing idle** kell; protokoll-hibára **`dp_resync` + retry** (tétlen szünettel old a beragadt vonal).
- Sebesség: bring-up 300 kHz, adatfázis `freq=0`; a flash alatt **csendes log** (INFO) és **kijelző progress-throttle** (250 ms) — utóbbi volt a legnagyobb rejtett lassító.

Kész és fordul: `swd_phy`, `adiv5`, `cortexm_debug`, `flm_runner`, `flm_blobs` (80 algo), `target_db` (80 STM32 DEV_ID), `prog_session` (end-to-end), `storage_lfs` (+ `storage_src` forrás-feloldó), `storage_usb` (USB MSC, Kconfig-gated), `display_lcd` (ILI9488+GT911+LVGL), `input_enc`, `ui`, `target_serial`, `target_state`, `net_wifi`, `web_ui`, `ftp_srv`. Tool: `tools/flm_extract.py`.

**USB pendrive forrás (✅ HW-IGAZOLT, Kconfig-gated, default KI):** valódi sticken működik (mount → lista a stickről → forrásváltás). `CONFIG_USB_MSC_HOST_ENABLE=y` esetén a bedugott FAT32 stick a `/usb` alá mountolódik (`storage_usb`: usb_host_msc + esp_vfs_fat, hot-plug), és ha van stick, a firmware (`/usb/fw`) + config (`/usb/cfg`) lista (OLED + web) a **stickről** jön — a belsőt elrejti; kihúzva visszaáll. A stick gyökerében `fw/` (és `cfg/`) mappa kell. A forrásválasztás a `storage_src` rétegen megy (prefix-alapú diszpécs LFS↔USB + a megfelelő lock). A **www** és az **FTP** szándékosan végig a belső LFS-en marad; a forrásfájl a flash/WiFi-pause ELŐTT teljesen PSRAM-ba olvasódik (kihúzás flash közben nem tör). **Mount-minta (FONTOS):** a MSC connect-callbackből NEM hívható `msc_host_install_device` (deadlock — a driver háttér-taszkjában fut); a callback csak sorba tesz, külön `usb_msc` task mountol. **Pad-ütközés:** OTG host = GPIO19/20 native USB → a másodlagos USB-Serial/JTAG konzolt KI kell kapcsolni (`CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`), konzol UART0-n, flash/monitor külső USB-UART-ról (GPIO43/44); fordítási `#error` véd, ha mégis bent maradna. Bekapcsoláshoz: `sdkconfig.usb` fragment. Default kikapcsolva = nulla viselkedésváltozás (natív USB flash/monitor megmarad). Részletek: a terv 18. szekciója.

**Flash-loaderek (FONTOS eltérés a tervtől):** a terv CMSIS `.FLM`-et írt, de a gépen a **STM32CubeProgrammer `.stldr`** (ST loader-ABI) loaderei vannak meg DEV_ID szerint nevezve (`bin/FlashLoader/0x431.stldr` stb.), ezért az **ST ABI**-t kötöttük be. Különbségek a CMSIS-hez: `Init/Write(addr,size,buf)/SectorErase(start,end)/MassErase/Verify`, **siker = 1** (nem 0), `StorageInfo` leíró. A `flm_algo_t` mindkét ABI-t leírja (`abi`, `success_ret`, `load_addr`, abszolút belépési pontok). Jelenleg **mind a 80** target_db DEV_ID-hez van loader generálva (`flm_generated.c`, ~159 KB — a `FlashLoader/0x4xx.stldr` teljes belső-flash készlet; nonSecure-duplikátumok és nem-Cortex-M / MP ID-k kihagyva), az ST `Verify` opcionális (a runner visszaolvasással verifikál). A DEV_ID→család/IDCODE-cím/flash-size reg/RDP leképezés OpenOCD (`stm32*.c`) + stlink + ST RM keresztellenőrzéssel készült; **futásidőben a flash-méret NEM kritikus** (pontos DEV_ID-egyezésnél a loader StorageInfo-ja adja a geometriát). Új IDCODE-címek: **0xE0044000** (M33: L5/U5/H5/WBA) és **0x5C001000** (H7); a detektálás csak akkor fogad el egy DEV_ID-t, ha a beolvasott cím egyezik a tábla `dbgmcu_idcode_addr` mezőjével. RDP az új/bizonytalan családoknál (H7/H5/L0/L1/L5/U0/U5/WB/WL/WBA) szándékosan **NONE** (egy rossz címről olvasott szemét hamis „RDP≥1"-ként blokkolná a nyitott chip flashelését; a flashelhetőséget nem érinti).
- **Újrageneráláshoz** (más gép / több típus): `tools/flm_extract.py <id>.stldr ... -o components/flm_blobs/flm_generated.c --header components/flm_blobs/flm_generated.h` (pyelftools kell). A nyers `.stldr` ST-proprietary → **NEM kerül a repóba**, csak a generált C tömbök.

## Web-UI és tesztek

- **Web-UI**: `data/www/{index.html,app.css,app.js}` (vanilla, sötét téma, magyar). A `main/CMakeLists.txt` `littlefs_create_partition_image(storage ../data FLASH_IN_PROJECT)`-tal a `data/` a `storage` partícióra kerül (`idf.py flash` felírja). Fájllista/feltöltés/flash/cfg + élő WS (`type:"state"`/`"prog"`).
- **Host tesztek** (`tests/`, PC-n futnak, nincs HW): `python -m unittest discover -s tests -v` (IDF python; pyelftools). 11 teszt: flm_extract ST-parser valódi `.stldr`-en, `flm_generated`↔`target_db` konzisztencia, CRC16 golden-vektorok. Natív host C fordító nincs → a C-logika golden-vektorral/konzisztenciával fedett.

**HW-validáció — ✅ A0 KÉSZ (F030/0x440, F407/0x413, G0):** a SWD/FLM mag valódi STM32-n bizonyítva több célon (DPIDR/IDCODE, connect-under-reset SYSRESETREQ-kel, ST `Init`/erase/program/read-back verify), end-to-end flash + verify átment, ~3,1 s (F030). Részletek: [reference/MILESTONE_SWD_HW.md](reference/MILESTONE_SWD_HW.md).

**Hátralévő:**
- **USB MSC:** ✅ alap HW-validáció kész (mount + lista a stickről + forrásváltás). Hátra: stickről induló SWD-flash + verify élesben, és `.cfg` pull/push a stickre (a kód kész, csak még nem hajtottuk végre HW-n).
- **Multi-család HW-teszt** valódi célokon (C1): eddig **F030 (0x440), F407 (0x413) és G0** igazolt élesben. A paletta **80 DEV_ID-re** bővült (F0/F1/F2/F3/F4/F7/H7/H5/G0/G4/L0/L1/L4/L5/U0/U5/WB/WL/WBA/C0) — az új családok DEV_ID-jai (különösen az M33 @ 0xE0044000 és H7 @ 0x5C001000 IDCODE-ágak, valamint a quad/256-bit programozási egységek) HW-n még nem igazoltak; ezeket valódi célon ellenőrizni kell.
- **RDP-címek az új családoknál** (H7/H5 OPTSR, L0/L1, L5/U0/U5/WBA OPTR@0x40022040, WB/WL OPTR@0x58004020): a címek kikutatva és a `target_db.c` megjegyzéseiben dokumentálva, de szándékosan `RDP_REG_NONE` (HW-validáció nélkül egy téves olvasás hamis blokkolást okozna). HW-teszt után bekapcsolhatók.
- (Opcionális, nem blokkoló) Program-fázis FLM-hívás regiszter-setup optimalizálás; SWCLK-max újramérése más bekötés/cél esetén.
- **Kész (HW nélkül lezárt):** web token-auth (`WEB_UI_TOKEN`, default nyílt), UI „Cél info", RDP-detektálás + hibakód-taxonómia, opcionális HW-nRST ág (`CONFIG_CORTEXM_HW_NRST`, default off), CI (`.github/workflows/build.yml` — jelenleg csak `workflow_dispatch`, lásd lent).

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
  display_lcd/    # ILI9488 480×320 SPI TFT + GT911 touch + LVGL v9 (port-taszk, UI képernyők)
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

**Szabadon kiosztható:** GPIO1–18, 21, 38–42, 47, 48 (ebből az AVR ISP már foglal néhányat — lásd lent).

**Javasolt kiosztás (ezen a branchen):** SWCLK=GPIO4, SWDIO=GPIO5, nRST=GPIO6 (OD+pullup — **fenntartva az ESP-n, a célhoz NEM kötjük**), UART TX=GPIO17, UART RX=GPIO18, Enkóder A=GPIO10, B=GPIO11, SW=GPIO12, Vref ADC=GPIO1, táp EN=GPIO13, LED=GPIO14.

**Kijelző — ILI9488 480×320 SPI TFT:** SCLK=GPIO9, MOSI=GPIO8, CS=GPIO38, DC=GPIO39, RST=GPIO40, BL(háttérvilágítás)=GPIO41. **GT911 kapacitív touch (I2C):** SDA=GPIO47, SCL=GPIO48, INT=GPIO42, RST=GPIO2. *(Az SSD1306 OLED I2C bekötés — SDA=8/SCL=9 — a `main` ágon érvényes; itt a TFT/touch váltja.)*

**AVR ISP (ATtiny13 stb. — külön interfész, `avr_isp` komponens):** SCK=GPIO15, MOSI=GPIO16, MISO=GPIO7, RESET=GPIO21 (aktív-alacsony, programozás alatt lehúzva). Bit-bang SPI ISP; forrás `.hex` (Intel HEX) vagy `.bin` a LittleFS-ből. Lábak Kconfig-gal állíthatók (`CONFIG_AVR_ISP_*_GPIO`).

> **A cél-csatlakozó nRST nélkül:** SWCLK, SWDIO, GND, (VTARGET sense). A reset vonalra nincs szükség — a csatlakozás SYSRESETREQ-kel megy (lásd lejjebb).

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

**Teljes naplózás (UART-konzol):** alap szint DEBUG, VERBOSE befordítva; a `main` a SWD/FLM tag-eket (`swd_phy/adiv5/cortexm/flm_runner/prog_session/...`) VERBOSE-ra emeli. Boot-banner + rendszer-infó + **boot-idői SWD önteszt** (`CONFIG_BRINGUP_SELFTEST=y`): induláskor detektálja a célt és kiírja DPIDR/DEV_ID/flash/RDP. Periodikus ismétlés: `CONFIG_BRINGUP_SELFTEST_PERIODIC`. A teljes folyamat `idf.py monitor`-ban követhető.

**Build-környezeti buktatók (megtapasztalt):**
- Az IDF tools eredetileg csak riscv targetre volt telepítve → az `export.ps1` nem tette az xtensa fordítót a PATH-ra ("Did not find file Compiler/-ASM"). Megoldás: egyszer lefuttatva `install.ps1 esp32s3`, utána jó.
- `partitions.csv`: **ne legyen sor végi `# komment`** az adatsorok után — a 6. oszlopot flag-nek veszi. A komment külön sorba kerüljön.
- `nincs gh` telepítve; remote: `origin = https://github.com/eniac77/ESP32S3_SWD_PROG.git`. Commit + push **csak zöld build után**.

## Fontos technikai részletek (a tervből)

- **Csatlakozás nRST NÉLKÜL (fontos megkötés):** a cél áramkörökön a reset (nRST) láb **nem elérhető**, ezért a programozás tisztán SWD-n megy: SWD bring-up → `DHCSR` halt → `DEMCR VC_CORERESET` → `AIRCR SYSRESETREQ` (szoftveres reset) → halt a reset-vektoron → vektor-catch törlése. A `C_DEBUGEN` a debug-tápdomainben marad, a SYSRESETREQ nem törli. Korlát: alvó/lockolt/RDP-s cél, amit csak hardveres nRST-vel lehetne ébreszteni, így nem mindig érhető el — normál programozáshoz viszont elég. Az ESP **nRST=GPIO6 lába megmarad** (fenntartva más célra), de a cél-programozás nem használja.
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

**Kijelző/enkóder port:** ezen a `feat/ili9488-lvgl` branchen a kijelző **ILI9488 480×320 SPI + GT911 touch + LVGL v9** — a részletes port-terv: [reference/ILI9488_LVGL_port.md](reference/ILI9488_LVGL_port.md). A branch az OLED `main`-ből ágazik. *(Történeti referencia az eredeti OLED-porthoz — SSD1306/SH1106 driver, 5×7 ASCII + Swis721 szám-font, enkóder/gomb logika az NX80TESTER_AVR projektből: [reference/NX80_display_port.md](reference/NX80_display_port.md). Ez a `main` ág OLED-változatára vonatkozik.)*
