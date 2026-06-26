# ESP32-S3 önálló SWD programozó + konfigurátor — megvalósítási terv (ESP-IDF)

**Modul:** ESP32-S3-**N16R8** (16 MB flash, 8 MB **octal** PSRAM).

**Cél:** PC nélküli készülék, amely
1. **LittleFS-ből** beolvas egy firmware-t (`.bin`) és **SWD**-n (SWCLK/SWDIO) felprogramozza a célként csatlakoztatott STM32-t (családok: F0, F1, F3, F4, F7, L0, L1, L4, G0), a flash-kódot **CMSIS `.FLM`** blobokból a cél RAM-jában futtatva (RAM flash loader);
2. **soros vonalon** (UART/RS485) kommunikál a cél STM32 *futó alkalmazásával*, és bináris **`.cfg`** fájlokból fel-/letölti a konfigurációt, valamint élő adatokat olvas;
3. helyileg **gombos enkóder + SSD1306 OLED**-ről kezelhető (fájlválasztás görgetett listából, cél-típus, %-os állapot);
4. WiFi-n **webszerver UI**-t és **FTP-szervert** ad a LittleFS eléréséhez (fájl fel-/letöltés, célkonfig, élő adatok).

> A két célinterfész fizikailag is külön van: **SWD** = firmware-flashelés (mag megállítva); **UART/RS485** = konfig + élő adat a *futó* célalkalmazással. Mindkettő a cél-csatlakozóra megy.

> **USB host (pendrive)**: egyelőre **félretéve**, lásd a 18. (Future) szekciót — a forrás most LittleFS.

---

## 1. Architektúra áttekintés

```
            WiFi (STA/AP)
        ┌───────┴────────┐
   [HTTP webUI]      [FTP szerver]
        │                 │
        └──────► LittleFS (/lfs)  ◄── fw/*.bin, cfg/*.cfg, www/*
                     │
   [gombos enkóder]  │   ┌─────────────── prog_session (SWD/FLM) ──► SWCLK/SWDIO/nRST ─► cél STM32 (flash)
        │            ▼   │
   [SSD1306 OLED] ◄─ UI/state ─┐
        ▲                      └─ target_serial (UART/RS485) ◄─► TX/RX/DE ─► cél STM32 (futó app: .cfg, élő adat)
        └──────────── live target_state ◄───────────────────────┘
```

- **Forrás:** LittleFS a belső 16 MB flashen (firmware + cfg + webUI assetek).
- **Programozó út (SWD):** `swd_phy` (dedic_gpio) → ADIv5 → Cortex-M debug → FLM-runner → célflash. (Változatlan mag a korábbi tervhez képest.)
- **Konfig út (soros):** `target_serial` keretezett bináris protokoll → `.cfg` fel/letöltés + élő adat → közös `target_state` modell.
- **Helyi UI:** enkóder+gomb → eseménysor → `ui` taszk → OLED.
- **Távoli UI:** `httpd` (REST + WebSocket) + `ftp` a LittleFS-re.

### A platformfüggetlen mag elve (változatlan)
A SWD/FLM mag kizárólag SWD-regisztertranzakció; az egyetlen platformspecifikus rész a 4-függvényes PHY HAL:
```c
void     swd_phy_seq_out(uint32_t bits, int n);  // n bit LSB-first
uint32_t swd_phy_seq_in(int n);
void     swd_phy_dir(bool drive);                // turnaround
void     swd_phy_idle(int clocks);               // idle/reset SWDIO=1
```

---

## 2. Hardver — N16R8 lábkiosztás

### 2.1 N16R8 fenntartott lábak (FONTOS)
- **Octal PSRAM (R8):** **GPIO33–GPIO37 foglalt**, általános célra **nem használható**.
- **Belső flash SPI:** GPIO26–32 foglalt.
- **Strapping:** GPIO0, 3, 45, 46 — kerüld vagy óvatosan.
- **GPIO19/20:** native USB pad — most **fenntartva** a jövőbeli USB hosthoz (18. szekció), ne foglald le másra.

Szabadon kiosztható (példa): GPIO1–18, 21, 38–42, 47, 48.

### 2.2 Javasolt kiosztás

| Funkció | Láb (példa) | Periféria/megjegyzés |
|---|---|---|
| SWCLK (out) | GPIO4 | `dedic_gpio` bundle |
| SWDIO (bidir) | GPIO5 | `dedic_gpio` out+in, turnaround |
| nRST (out, OD) | GPIO6 | open-drain + pullup, connect-under-reset |
| Cél UART TX | GPIO17 | `target_serial` (UART1, sima 3.3 V) |
| Cél UART RX | GPIO18 | `target_serial` (sima 3.3 V) |
| (GPIO15 szabad) | GPIO15 | tartalék (korábbi RS485 DE elhagyva) |
| OLED I2C SDA | GPIO8 | SSD1306 128×64 |
| OLED I2C SCL | GPIO9 | I2C0, 400 kHz |
| Enkóder A | GPIO10 | ISR (vagy PCNT) |
| Enkóder B | GPIO11 | irány |
| Enkóder SW (gomb) | GPIO12 | ISR + debounce, short/long |
| Cél Vref érzékelés | GPIO1 (ADC1) | feszültség-jelenlét + szint |
| Cél táp EN (opc.) | GPIO13 | load switch, árammért 3.3 V |
| Státusz LED | GPIO14 / WS2812 | fázis/hiba kijelzés |

### 2.3 Cél-csatlakozó
`SWCLK, SWDIO, nRST, UART_TX, UART_RX, GND, VTARGET(sense/feed)` (sima 3.3 V UART, RS485 nélkül).
MVP: **3.3 V-os cél**, SWCLK/SWDIO közvetlenül soros ~33–100 Ω-mal. Egyéb Vdd-hez SWDIO bidir szintfordító. A cél Vdd-jét ADC-vel ellenőrizd, mielőtt tranzakciót indítasz.

### 2.4 sdkconfig kulcs-beállítások (N16R8)
```
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y          # octal PSRAM (R8)
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y        # heap-be vonva
CONFIG_PARTITION_TABLE_CUSTOM=y   # partitions.csv
```

---

## 3. Particionálás (16 MB)

`partitions.csv` (kiindulás):
```
# Name      Type  SubType   Offset     Size
nvs         data  nvs       0x9000     0x6000
phy_init    data  phy       0xf000     0x1000
factory     app   factory   0x10000    0x300000   # 3 MB app
storage     data  littlefs  0x310000   0xCF0000   # ~13 MB LittleFS (fw + cfg + www)
```
- LittleFS-ben: `/lfs/fw/*.bin`, `/lfs/cfg/*.cfg`, `/lfs/www/*` (webUI assetek).
- **Miért LittleFS** (FAT/SPIFFS helyett): power-fail biztos, wear-leveling, könyvtárak — mezei eszközhöz ez kell.

---

## 4. Komponens-struktúra (ESP-IDF)

```
esp32s3-swd-prog/
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv
├── main/                         # init + taszkok indítása
├── components/
│   ├── swd_phy/                  # dedic_gpio HAL (platformspecifikus)
│   ├── adiv5/                    # DP/AP transport, switch, ACK/parity
│   ├── cortexm_debug/            # halt/reset, mem R/W, DCRSR/DCRDR
│   ├── flm_runner/               # call_function + FLM futtatás
│   ├── flm_blobs/                # generált C tömbök (PrgCode/PrgData/DevDsc)
│   ├── target_db/                # DEV_ID → FLM + flash-size reg
│   ├── prog_session/             # SWD programozás orchestráció
│   ├── target_serial/            # UART/RS485 híd: .cfg fel/le, élő adat
│   ├── target_state/             # közös élő-adat modell (UI + WS forrása)
│   ├── storage_lfs/              # LittleFS mount + fájl-API + lock
│   ├── input_enc/                # enkóder ISR/PCNT + gomb → eseménysor
│   ├── display_oled/             # SSD1306 driver + UI képernyők
│   ├── net_wifi/                 # STA/AP, provisioning, mDNS
│   ├── web_ui/                   # esp_http_server: REST + WebSocket
│   └── ftp_srv/                  # FTP a LittleFS fölött
├── tools/
│   └── flm_extract.py            # build-time .FLM → C tömb
└── flm_packs/                    # vendored .FLM források (ST DFP)
```

### Függőségek
```bash
idf.py add-dependency "joltwallet/littlefs"     # LittleFS VFS
# esp_http_server, esp_wifi, mdns, driver/i2c, driver/uart,
# driver/dedic_gpio, driver/pulse_cnt: beépített
# OLED: u8g2 (SSD1306 + fontok + menü) drop-in lehetőség, VAGY a saját font/lib-ed
# FTP: LittleFS/VFS-háttér FTP komponens (lásd 12.)
```

---

## 5. SWD PHY — `dedic_gpio`  *(változatlan mag)*

`gpio_set_level()` lassú; a `dedic_gpio` az S3 dedikált GPIO-instrukcióival ~single-cycle. SWCLK+SWDIO kimeneti bundle, SWDIO külön bemeneti bundle; turnaroundnál a SWDIO pad irányát váltod. Kezdj ~200–500 kHz-en, bring-up után told fel. Az SWD szinkron → clock-stretching mindig OK, a FreeRTOS/WiFi jitter nem töri el; a PHY-t mégis tartsd külön taszkban, kritikus szakaszt rövidre.

```c
static inline void clk_low(void)  { dedic_gpio_bundle_write(out, SWCLK_MASK, 0); }
static inline void clk_high(void) { dedic_gpio_bundle_write(out, SWCLK_MASK, SWCLK_MASK); }
void swd_phy_seq_out(uint32_t bits, int n){
    for (int i=0;i<n;i++){
        dedic_gpio_bundle_write(out, SWDIO_MASK, ((bits>>i)&1)?SWDIO_MASK:0);
        clk_low(); clk_high();
    }
}
```

---

## 6. ADIv5 transport  *(változatlan mag)*

- **Bring-up:** ≥50 SWCLK SWDIO=1 → JTAG-to-SWD switch `0xE79E` LSB-first (wire: `0x9E 0xE7`) → ≥50 SWCLK → DPIDR olvasás. (A 9 család nem igényel dormant szekvenciát.)
- **Csomag:** request 8 bit; turnaround; ACK `001`=OK/`010`=WAIT/`100`=FAULT; data 32 bit + paritás. WAIT→retry, FAULT→ABORT.
- **DP:** DPIDR/ABORT(0x0), CTRL/STAT(0x4), SELECT(0x8), RDBUFF(0xC). Debug power-up: `CDBGPWRUPREQ|CSYSPWRUPREQ`.
- **AHB-AP:** CSW(0x0)/TAR(0x4)/DRW(0xC)/IDR(0xFC). CSW: Size=Word(2)+AddrInc=Single(1), reset-default bitek read-modify-write-tal. Posted read → utolsót RDBUFF-ból.
- **Primitívek:** `mem_write32/read32`, `mem_write_block/read_block` (TAR auto-inc) — családfüggetlen.

---

## 7. Cortex-M debug  *(változatlan mag)*

| Funkció | Cím | Művelet |
|---|---|---|
| Halt | DHCSR `0xE000EDF0` | `0xA05F0003`, poll `S_HALT` |
| Resume | DHCSR | `0xA05F0001` |
| Reg W | DCRSR `0xE000EDF4`/DCRDR `0xE000EDF8` | DCRDR←val, DCRSR←(REGSEL\|1<<16), poll `S_REGRDY` |
| Reset | AIRCR `0xE000ED0C` | `0x05FA0004` |
| Reset+halt | DEMCR `0xE000EDFC` | `VC_CORERESET` + connect-under-reset |

REGSEL: R0–R12=0..12, SP=13, LR=14, PC=15, **xPSR=16**.
**Connect-under-reset:** nRST assert → SWD bring-up → DEMCR VC_CORERESET → nRST release → halt a reset-vektoron. Kötelező.

---

## 8. FLM-runner  *(változatlan mag)*

RAM layout 0x20000000-ról: `[PrgCode][PrgData][BKPT-szó][stack↓][buffer]`.
ABI (FlashOS.h): `Init/UnInit/EraseSector/EraseChip/ProgramPage/Verify`, mind 0=siker; a write-granularitás a `ProgramPage`-en belül.

```c
int flm_call(uint32_t pc,uint32_t r0,uint32_t r1,uint32_t r2,uint32_t r3){
    reg_write(R0,r0);reg_write(R1,r1);reg_write(R2,r2);reg_write(R3,r3);
    reg_write(SP,stack_top);          // 8-byte align
    reg_write(LR,bkpt_addr|1);        // Thumb
    reg_write(PC,pc|1);               // Thumb
    reg_write(xPSR,0x01000000);       // T-bit! enélkül INVSTATE fault
    cortexm_resume(); poll_until_halt(timeout);
    uint32_t ret; reg_read(R0,&ret); return (int)ret;
}
```
**Folyamat:** connect-under-reset → DEV_ID → FLM kiválaszt → PrgCode/PrgData RAM-ba → flash-size kiolvas → Init(erase) → EraseSector× → Init(prog) → page-enként (buffer RAM-ba + ProgramPage) → verify → reset&run. A progress-callback innen táplálja az OLED/web %-ot.

---

## 9. FLM-kinyerő pipeline — `tools/flm_extract.py`  *(változatlan)*

`.FLM` = ARM ELF; `PrgCode`/`PrgData`/`DevDsc` szekciók pyelftools-szal kinyerve → C tömb + `FlashDevice` struct/`flm_descriptor_t`. CMake `add_custom_command`-ben regenerál. FLM-források: ST DFP / STM32CubeProgrammer; family-buildhez a legkisebb RAM-ú taghoz méretezett változat.

---

## 10. Cél-adatbázis — `target_db`  *(változatlan)*

| Család | Mag | DBGMCU IDCODE | Prog granularitás / megjegyzés |
|---|---|---|---|
| F0 | M0 | `0x40015800` | half-word; kis RAM-ú variánsok |
| F1 | M3 | `0xE0042000` | half-word |
| F3 | M4 | `0xE0042000` | half-word |
| F4 | M4 | `0xE0042000` | PSIZE konfigurálható, nagy sectorok |
| F7 | M7 | `0xE0042000` | ITCM/AXIM cím-leképezés íráskor |
| L0 | M0+ | `0x40015800` | page flash, half-page write |
| L1 | M3 | `0xE0042000` | EEPROM + page flash |
| L4 | M4 | `0xE0042000` | double-word (64-bit), dual-bank |
| G0 | M0+ | `0x40015800` | double-word |

`dev_id` + flash-size reg → méret-variáns (a flash-size reg címe családspecifikus, vedd fel a táblába).

---

## 11. Forrás-tár — `storage_lfs` (LittleFS)

```c
esp_vfs_littlefs_conf_t c = { .base_path="/lfs", .partition_label="storage",
                              .format_if_mount_failed=true };
esp_vfs_littlefs_register(&c);
```
- Könyvtárak: `/lfs/fw`, `/lfs/cfg`, `/lfs/www`.
- **Egyidejű hozzáférés:** a webUI, az FTP és a `prog_session` is nyúlhat a fájlokhoz → **közös mutex** a fájlműveletekre (LittleFS nem feltétel nélkül thread-safe ugyanarra a fájlra). Egy `storage_lfs` API mögé zárva, minden írás/lista a lock alatt.
- Firmware kiválasztásnál a `.bin`-hez társíthatsz egy `.meta`-t (cél-család, base-cím, CRC) — vagy konvenció (`0x08000000`, auto-detektált család).

---

## 12. Távoli elérés — `net_wifi`, `web_ui`, `ftp_srv`

### 12.1 WiFi
- **APSTA**: STA a meglévő hálózatra (creds NVS-ből), AP-fallback ha STA nem jön össze (mezei használat). mDNS: `swdprog.local`.
- Provisioning: NVS-be mentett creds, vagy egyszerű AP-s konfigoldal.

### 12.2 Webszerver (`esp_http_server`)
- Statikus UI a `/lfs/www`-ból.
- REST:
  - `GET /api/files` — LittleFS lista (fw/cfg)
  - `POST /api/upload` — multipart, fw/cfg feltöltés
  - `GET /api/download?path=` / `DELETE /api/file?path=`
  - `POST /api/program {file}` — SWD flash indítás (progress WS-en)
  - `POST /api/cfg/push {file}` / `GET /api/cfg/pull` — célkonfig fel/le (soros hídon át)
- **WebSocket `/ws`** — élő adat (target_state) + programozási progress + log streamelése.
- **Biztonság:** mivel a felület képes célt törölni/flashelni, legalább opcionális Basic Auth / token; AP-módban erősen ajánlott.

### 12.3 FTP (`ftp_srv`)
- LittleFS/VFS-háttérrel; firmware és `.cfg` drag-drop fel/letöltése.
- Plaintext → csak LAN. A `storage_lfs` lockját használja (ne ütközzön a webUI-val).
- Egyszerű kliens-limit (1–2 session), passzív mód.

---

## 13. Cél-soros híd — `target_serial` + `target_state`

A *futó* cél STM32-vel UART (vagy RS485 fél-duplex) felett, a SWD-től **függetlenül**.

### 13.1 Transzport
- **UART1, sima 3.3 V** (TX/RX), állítható baud. Nincs RS485/DE — pont-pont a céllel, közös GND. (Ha valaha izolált/RS485 kellene, a transzport mögé tehető, az alkalmazás-réteg nem változik.)
- Keretezett bináris protokoll (pluggable, a TE protokollodhoz igazítva):
  `SOF | LEN | CMD | PAYLOAD | CRC16` — időtúllépés + retry, frame-szinkron keresés. Fél-duplex logika nem kell, a UART full-duplex.

### 13.2 `.cfg` fájlok
- A `.cfg` **opak bináris blob** (a cél értelmezi). A híd csak keretekbe tördelve mozgatja:
  - **letöltés:** `cfg_pull` → célkonfig kiolvas → `/lfs/cfg/<név>.cfg`
  - **feltöltés:** `cfg_push` → `/lfs/cfg/<név>.cfg` → célbe írás, ack-elve
- A konkrét parancs-ID-kat (GET_CONFIG/SET_CONFIG/COMMIT stb.) egy vékony „protokoll-adapterben" definiáld — ez a te alkalmazás-protokollod (a binárisszerver-stílusú GET_STATUS/SET-param mintádra).

### 13.3 Élő adat — `target_state`
- A cél periodikus státusz-frame-jei → közös `target_state` modell (mutex/atomic snapshot).
- Fogyasztók: OLED (helyi kijelzés) és webUI WebSocket (élő grafikon/számok). Egy forrás, két nézet.

---

## 14. Helyi bevitel — `input_enc` (gombos enkóder)

### 14.1 Enkóder (megszakítás alapú, ahogy kérted)
- A/B ISR: A élre megszakítás, B olvasásból irány; per-detent debounce (~1–5 ms) vagy 2-bites kvadratúra állapotgép a fél-/teljes-lépés szűréshez.
- Eredmény: `+1/−1` tick → eseménysor (`xQueueSendFromISR`).
- **Alternatíva (ajánlott robusztusságra):** a **PCNT** periféria hardveres kvadratúra-dekódot + glitch-filtert ad, ISR-jitter nélkül. A `dedic_gpio` SWD mellett ez nullára viszi az enkóder-melót. Az API ugyanaz marad (tick-ek a sorba), csak a forrás más — később dropp-in cserélhető.

### 14.2 Gomb (a tengely nyomógombja)
- Külön GPIO ISR + debounce, **short/long press** megkülönböztetés (timer a lenyomás hosszára) → `BTN_SHORT`/`BTN_LONG` esemény.

### 14.3 Eseménymodell
`enc_event_t { ENC_CW, ENC_CCW, BTN_SHORT, BTN_LONG }` → egy queue → `ui` taszk. A bevitel teljesen leválasztva a renderelésről.

---

## 15. Kijelző + helyi UI — `display_oled` (SSD1306 128×64, I2C)

### 15.1 Driver
- I2C0 @ 400 kHz, SSD1306 128×64. A font/grafikus réteg **pluggable**: első körben a **te font/lib-ed**, vagy **u8g2** (SSD1306 driver + fontok + menü-primitívek) drop-in. A panel-driver absztrakt marad → később nagyobb kijelzőre cserélhető.
- Renderelés külön `ui` taszkban, csak változásra/~15–30 fps; ne blokkolja az SWD-t (az amúgy is clock-stretch-tűrő).

### 15.2 Képernyők / flow
1. **Idle/status:** WiFi (STA/AP, IP), cél detektálva? (típus), soros link állapot.
2. **Főmenü** (enkóder görget, short=belép, long=vissza): `Program firmware` / `Cél konfig` / `Élő adat` / `Beállítások`.
3. **Program firmware:**
   - görgetett **fájllista** `/lfs/fw`-ből (enkóder), short=kiválaszt;
   - SWD connect → **detektált cél-típus** kiírása (DEV_ID→név) + flash méret;
   - megerősítés (long=mégse) → flash, **%-os progress bar** fázis-címkével (Erase/Program/Verify);
   - eredmény (OK/HIBA + kód).
4. **Cél konfig:** `.cfg` push/pull a `/lfs/cfg`-ból, állapot/ack kiírás.
5. **Élő adat:** `target_state` mezők (számok/mini-bar), enkóderrel lapozható.
6. **Beállítások:** SWCLK sebesség, baud/RS485, WiFi mód, stb.

---

## 16. Taszk-modell és erőforrások

| Taszk | Felelősség | Megjegyzés |
|---|---|---|
| `prog_task` | SWD/FLM session | magas prio kritikus szakaszban, de stretch-tűrő |
| `serial_task` | UART/RS485 RX/TX, frame-elés | `target_state` frissítés |
| `ui_task` | OLED render + enkóder/gomb fogyasztás | csak változásra |
| `httpd` | REST + WS | esp_http_server adja |
| `ftp_task` | FTP session(ök) | storage lock |
| `net_task` | WiFi STA/AP, mDNS felügyelet | reconnect |

- PSRAM (8 MB): a `.bin` puffer, webUI buffer, élő-adat history ide.
- Megosztott erőforrás-lockok: `storage_lfs` (fájl), `target_serial` (vonal), `swd` (cél-csatlakozó — SWD és serial **ne** egyszerre ugyanazon a célon, ha közös lábakon osztoznának; itt külön lábakon vannak, de a session-logika tiltsa a flash+config egyidejűségét).

---

## 17. Fázisos ütemterv

**A. track — SWD/FLM mag (a kritikus út, bench + UART-log)**

| MK | Tartalom | Kész-kritérium |
|---|---|---|
| A0 | `swd_phy` (dedic_gpio) + ADIv5 line reset/switch | DPIDR + IDCODE LA-val igazolva |
| A1 | AHB-AP mem R/W (single+block) | ismert reg-ek visszaolvasva |
| A2 | halt/reset/connect-under-reset, core reg | cél megáll reset-vektoron |
| A3 | `flm_call` triviális RAM-rutinnal | R0 a várt értéket adja |
| A4 | `flm_extract.py` + F4 (Black Pill cél) | Init/EraseSector 0=OK |
| A5 | teljes erase+program+verify hardcode bufferből | F4 felprogramozva, verify OK |

**B. track — platform (párhuzamosan fejleszthető)**

| MK | Tartalom | Kész-kritérium |
|---|---|---|
| B0 | LittleFS mount + `storage_lfs` API + lock | fájl lista/olvasás/írás megy |
| B1 | `display_oled` + `input_enc` (enkóder ISR/PCNT + gomb) | görgetett menü + select működik |
| B2 | `net_wifi` (APSTA, mDNS) | elérhető `swdprog.local` |
| B3 | `web_ui` REST + statikus UI a /lfs/www-ból | fájl fel/le böngészőből |
| B4 | `ftp_srv` a LittleFS fölött | FTP-vel fw/cfg másolható |
| B5 | `target_serial` + `target_state` + `.cfg` push/pull | célkonfig fel/le, élő adat folyik |
| B6 | WS élő adat + programozási progress | webUI mutatja a %-ot/értékeket |

**C. integráció**

| MK | Tartalom | Kész-kritérium |
|---|---|---|
| C0 | `prog_session` ← fájl LittleFS-ből, OLED/web progress | stickless end-to-end flash a kijelzőről |
| C1 | `target_db` + multi-család | mind a 9 család referenciapanelen átmegy |
| C2 | RDP/robusztusság/hibakódok, lock-ütközések | doboz-kész működés |

> Sorrend-javaslat: **A0–A2** előbb (legkockázatosabb), de **B0/B1** korán hasznos (helyi UI + LittleFS a vakon-debug helyett). A web/FTP/serial a magtól függetlenül érlelhető.

---

## 18. Future — USB host (félretéve)

Később, ha pendrive-ról is kell tölteni: `espressif/usb_host_msc` + FATFS a GPIO19/20 native USB padon (ezért hagytuk szabadon). **Pad-ütközés:** OTG host módban a USB-Serial-JTAG nem használható ugyanazon a lábon → konzol **UART0**-ra (GPIO43/44). A stickhez külön 5 V VBUS (boost/load switch). A `storage` réteg már absztrakt (`storage_lfs` mintára egy `storage_usb`), így a `prog_session` forrása csak konfig kérdése lesz.

---

## 19. Élek és kockázatok

| Téma | Kezelés |
|---|---|
| **N16R8 octal PSRAM lábak** | GPIO33–37 foglalt; sose oszd ki perifériára |
| **Kis RAM-ú cél** (F030F4, L011) | FLM+stack+buffer beférjen; legkisebb tagra méretezett FLM |
| **RDP** | RDP1 → csak mass-erase után írható; RDP2 végleges. Option-byte külön |
| **Connect-under-reset** | kötelező; alvó/lockolt/WDG-s cél máskülönben megfekszik |
| **F7 ITCM/AXIM, L4/G0 double-word** | FLM kezeli; buffer-igazítás (8 byte) figyelendő |
| **LittleFS egyidejűség** | web+FTP+prog közös mutex; ne írj egy fájlt két helyről |
| **Soros vs SWD egyidejűség** | session-logika tiltsa a flash+config párhuzamot ugyanazon célon |
| **Cél UART szintek** | sima 3.3 V, közös GND; eltérő Vdd-jű célnál szintillesztés a TX/RX-en is |
| **WS/HTTP heap** | nagy fájlfeltöltés streamelve (ne a teljes fájl heapbe); PSRAM-buffer |
| **WiFi-jitter az SWD alatt** | SWD stretch-tűrő, de a kritikus szakaszt tartsd rövid taszkban |
| **Szintillesztés** | MVP 3.3 V cél; egyéb Vdd-hez SWDIO bidir szintfordító |

---

## 20. Referenciák a maghoz
- **pyOCD** (`flash`/`pack`): FLM-betöltés + `call_function` + CMSIS-pack parse minta (az `flm_extract.py`-hoz is).
- **probe-rs** (`flashing`): FLM-futtatás, jó target-modell.
- **Black Magic Probe** (`target/`): család-driverek + ADIv5 (GPLv3 — termékhez figyelni).
- **FlashOS.h** (ARM CMSIS-Pack): az FLM ABI definíciója.
- **u8g2**: SSD1306 + fontok + menü (ha nem saját lib).

### Következő lépés
A0–A2 a kritikus út; ezzel párhuzamosan B0+B1 (LittleFS + OLED/enkóder) ad korai helyi visszajelzést. A0-t F411 célon érdemes kezdeni (nagy RAM, jól dokumentált).
