# AVR UPDI — protokoll- és bring-up referencia

**Állapot:** implementálva (`components/avr_updi/`), **HW-n MÉG NEM IGAZOLT**, de a
protokoll-konstansok és a program-szekvenciák a Microchip **pymcuprog** forráshoz
(`serialupdi/constants.py`, `link.py`, `nvmp0.py`, `nvmp2.py`) **hitelesítve**.
Támogatott: **NVMCTRL v0** (tinyAVR/megaAVR 0/1/2) ÉS **v2** (AVR Dx: DA/DB/DD).
Ez a doksi a `avr_updi.c`-ben bekötött protokollt írja le, és a valódi célon
való bring-up-hoz ad checklistet — olvasd el UPDI-munka előtt, ahogy a
[MILESTONE_SWD_HW.md](MILESTONE_SWD_HW.md)-t SWD előtt.

## 1. Mire való, milyen célok

UPDI = **Unified Program and Debug Interface**: a modern AVR-ek egyvezetékes
program/debug interfésze. Célcsoportok:

- **tinyAVR 0/1/2-series** (ATtiny2xx/4xx/8xx/16xx/32xx) — **NVMCTRL v0**
- **megaAVR 0-series** (ATmega808/809/1608/1609/3208/3209/4808/4809) — **NVMCTRL v0**
- **AVR Dx/Ex** (AVR128DA/DB/DD, AVR Ex) — **NVMCTRL v2+** (más címek/parancsok, **még nincs bekötve**)

Nem UPDI: a klasszikus ATtiny13/ATmega328 (SPI-ISP → `avr_isp`), és az XMEGA (PDI → `avr_pdi`).

## 2. Fizikai bekötés (ezen a készüléken)

A meglévő „pines" programozó-headert használjuk; UPDI-hez **egyetlen** vonal kell:

| Jel | ESP láb | Megjegyzés |
|---|---|---|
| **UPDI_DATA** | **GPIO7** (a header MISO-ja) | egyvezetékes, kétirányú; idle = magas |
| GND | GND | közös föld |

- A target oldalon az UPDI a **RESET/UPDI lábon** van (fuse `RSTPINCFG=UPDI`, ez a gyári default a legtöbb tinyAVR-en).
- **Soros ~4,7 kΩ** ajánlott az ESP GPIO7 és a target UPDI láb közé (védi az egymásnak-feszülést, ha mindkét oldal hajtana).
- A vonal **open-drain + felhúzás** (a kódban GPIO7 `INPUT_OUTPUT_OD` + belső pullup); a cél is le tudja húzni (wired-AND).
- SCK/MOSI/RESET ESP-lábak UPDI módban **nem használtak**.

## 3. PHY — UART single-wire (8E2)

A UPDI keret **UART-szerű**: 1 start (0) + **8 adatbit LSB-first** + **páros paritás** + **2 stop** (8E2), idle magas. Az ESP **UART2**-t használjuk (UART0=konzol, UART1=`target_serial`), TX és RX **ugyanarra a GPIO7-re** kötve → half-duplex single-wire.

- **Echo:** minden elküldött bájt visszhangzik a saját RX-re. A `updi_send()` küldés után pontosan annyi bájtot olvas vissza és dob el, amennyit küldött; csak utána jön a cél válasza (ACK/adat).
- **Baud:** default **115200** (`CONFIG_AVR_UPDI_BAUD`). A UPDI alap UPDICLK-ja ~4 MHz, max baud ≈ UPDICLK/8; 115200 biztonságos. Feljebb a cél órajelétől függ.
- **BREAK:** ideiglenesen **4800 baud**-ra váltunk és egy `0x00`-t küldünk → ~1,9 ms low (≥12 bit), amit a UPDI bármilyen állapotból detektál és idle-re áll. Enable-kor **dupla BREAK**.

## 4. Link-réteg — utasítások

Minden utasítás-keret **SYNC (0x55)** bájttal kezdődik, utána az opkód, majd a cím/adat bájtok ugyanabban a frame-ben.

| Utasítás | Opkód | Forma | Válasz |
|---|---|---|---|
| **LDS** | `0x00 \| (a<<2) \| d` | SYNC, op, cím… | adat |
| **STS** | `0x40 \| (a<<2) \| d` | SYNC, op, cím… → **ACK** → adat… → **ACK** | 0x40 ACK ×2 |
| **LDCS** | `0x80 \| cs` | SYNC, op | 1 bájt (CS reg) |
| **STCS** | `0xC0 \| cs` | SYNC, op, érték | — |
| **KEY** | `0xE0` | SYNC, op, 8 kulcsbájt (**fordítva**) | — |
| LD/ST (ptr) | `0x20/0x60 \| …` | pointer-alapú, REPEAT-tel | (sebesség-opt, lásd 8.) |

- `a` = címméret (0=8b, 1=16b, **2=24b**), `d` = adatméret (0=8b). A kód egységesen **24-bites cím / 8-bites adat**-ot használ (`LDS24=0x08`, `STS24=0x48`).
- **ACK = 0x40** (STS cím- és adatfázis után).

### Control/Status (CS) regiszterek

| CS | Név | Szerep |
|---|---|---|
| 0x00 | STATUSA | UPDI revízió (felső nibble) |
| 0x01 | STATUSB | hibajelzők |
| 0x02 | **CTRLA** | guard time (GTVAL) — `0x06` = 2 ciklus (leggyorsabb) |
| 0x03 | CTRLB | |
| 0x07 | **ASI_KEY_STATUS** | aktív kulcsok (NVMPROG bit = `0x10`) |
| 0x08 | **ASI_RESET_REQ** | reset: `0x59` apply, `0x00` clear |
| 0x09 | ASI_CTRLA | |
| 0x0A | ASI_SYS_CTRLA | |
| 0x0B | **ASI_SYS_STATUS** | NVMPROG=`0x08`, UROWPROG=`0x04`, LOCKSTATUS=`0x01` |

## 5. Belépés NVM prog-módba

```
updi_break(); updi_break();          // dupla BREAK → UPDI idle
STCS CTRLA = 0x06                    // guard time = 2 ciklus
LDCS STATUSA                         // él-e a link? (UPDIREV olvasható)
LDCS ASI_SYS_STATUS → ha LOCKSTATUS  // lockolt chip: csak chip-erase kulccsal nyitható
KEY  "NVMProg "  (8 bájt, fordítva)  // a vonalra: ' ','g','o','r','P','M','V','N'
LDCS ASI_KEY_STATUS → NVMPROG(0x10)? // a kulcs aktív?
STCS ASI_RESET_REQ = 0x59; = 0x00    // reset-pulzus, hogy a kulcs érvénybe lépjen
LDCS ASI_SYS_STATUS → NVMPROG(0x08)? // prog-módban vagyunk?
```

**Kulcsok** (8 ASCII bájt, a KEY-nél fordított sorrendben küldve):
- NVMPROG: `"NVMProg "` — flash/EEPROM írás engedélyezése
- CHIPERASE: `"NVMErase"` — teljes törlés (lockolt chip nyitása); **még nincs bekötve**

> **KEY-fordítás (gyakori buktató):** a pymcuprog `link.key()` a kulcsot `reversed()`
> sorrendben küldi — `"NVMProg "` a vonalra `20 67 6F 72 50 4D 56 4E`. A `avr_updi.c`
> ezt megteszi (`rev[i] = key[7-i]`).

## 6. NVMCTRL v0 — programozási folyam (tinyAVR/megaAVR 0/1/2)

Címek a UPDI data-map-ben:

| Mit | Cím |
|---|---|
| NVMCTRL CTRLA | `0x1000` |
| NVMCTRL STATUS | `0x1002` (FBUSY=bit0, WRERROR=bit2) |
| SIGROW (signature) | `0x1100` |
| **Flash leképzés (base)** | **tinyAVR: `0x8000`**, **megaAVR0: `0x4000`** (eszközfüggő!) |

NVMCTRL parancsok (CTRLA-ba írva): NOP=0, WP=1, ER=2, **ERWP=3**, **PBC=4**, CHER=5.

Laponként:
```
STS NVMCTRL.CTRLA = PBC (4)          // page buffer clear
wait FBUSY=0
for b in 0..page_size:
    STS (flash_base + page*page_size + b) = data[b]   // a write a page-bufferbe megy
STS NVMCTRL.CTRLA = ERWP (3)         // erase + write page
wait FBUSY=0  (WRERROR=0!)
```

A **verify** visszaolvasás `LDS(flash_base + i)` + összevetés. Kilépés: `ASI_RESET_REQ` 0x59→0x00 (az alkalmazás fut, UPDI elenged).

> A `avr_updi.c` a pymcuprog-mintát követi: **chip erase (CHER) egyszer előre**, majd
> laponként **PBC → page-buffer feltöltés → WP** (nem ERWP-per-lap). A page-buffert
> **ST-pointer + word-írás** (`ST ptr` → `ST *(ptr++)` DATA_16) tölti, ami sokkal
> kevesebb bájt a vonalon, mint a per-bájt STS.

## 6b. NVMCTRL v2 — AVR Dx (DA/DB/DD)

Az AVR Dx **24-bites** UPDI-címzést használ, a flash a data-map-ben **`0x800000`**-nál,
lapméret **512 B**. Az NVMCTRL bázis ugyanúgy `0x1000`, de a **parancskészlet más**:

| Parancs | v2 CMD |
|---|---|
| NOCMD | `0x00` |
| FLASH_WRITE (page write) | `0x02` |
| FLASH_PAGE_ERASE | `0x08` |
| CHIP_ERASE | `0x20` |

- STATUS hibamező: **bit5:4 (`0x30`)**, nem a v0 egyetlen WRERROR-bitje (`0x04`).
- Folyam (pymcuprog `nvmp2`): chip erase előre, majd laponként **FLASH_WRITE → buffer feltöltés (word) → wait → NOCMD**. Nincs PBC; a parancs *előbb* megy, mint a mapelt-flash írások (command-then-write).
- Bekötött célok: AVR128DA48 (`1E 97 08`), AVR64DD32 (`1E 96 1A`), AVR128DB48 (`1E 97 0C`).

## 7. Signature-tábla (HW-n ELLENŐRIZENDŐ)

A `avr_updi.c` `UPDI_TABLE`-je a signature → név/flash/lap/flash_base leképzés.
Jelenlegi belépők (a signature-értékeket valódi chipen verifikálni kell):
ATtiny412/414/814/816/1614/1616/3216/3217, ATmega4808/4809. A flash_base
tinyAVR-en `0x8000`, megaAVR0-n `0x4000`.

## 8. Bring-up checklist (valódi célon)

A SWD-tanulság (csendben, lépésenként, loopback-kel bizonyítva) itt is áll:

1. **PHY loopback**: GPIO7 echo-ja jöjjön vissza önmagában (TX→RX a single-wire-ön) — bizonyítja, hogy az open-drain + matrix bekötés él, mielőtt a célt hibáztatnánk.
2. **STATUSA olvasás**: `updi_enable()` után az UPDIREV nibble nem 0/0xF → a cél válaszol.
3. **Signature**: `LDS 0x1100..0x1102` adja a vártat (pl. ATtiny1616). Ha 0xFF/0x00, a vonal/áram/baud a gyanús.
4. **Guard time / baud**: ha az echo-leürítés vagy az ACK timeout-ol, először a guard time-ot (CTRLA) és a baud-ot hangold; UPDI-nál a **csendes log** itt is fontos (a VERBOSE UART-spam ronthat).
5. **NVMPROG belépés**: `ASI_SYS_STATUS` NVMPROG bitje álljon be a reset-pulzus után.
6. **Egy lap** programozása + visszaolvasás, mielőtt a teljes képet futtatnád.
7. **WiFi**: a UPDI UART-alapú (nem bit-bang), így a SWD-nél látott RF-glitch valószínűleg nem gond — de ha mégis, a `net_wifi_radio_pause` mintát követni lehet.

## 9. Hátralévő / bővítés

- **AVR Dx (NVMCTRL v2)**: ✅ bekötve (lásd 6b). Hátra: **AVR Ex** (v3/v4), és a v2 signature-sor címe (`SIGROW` a Dx data-map-ben — a detect jelenleg `0x1100`-at olvas, ez Dx-en ELLENŐRIZENDŐ).
- **CHIPERASE kulcs** lockolt chiphez (`"NVMErase"`), és a `.cfg`/EEPROM kezelés.
- **RSD streaming** (még gyorsabb): a lap-fill jelenleg ACK-olt word-írás (`ST *(ptr++)` DATA_16 per word). A pymcuprog ACK-mentes RSD-blokkja (`CTRLA=0x88` ↔ `0x80`) tovább gyorsít — HW-validáció után érdemes.
- **UI-menü**: külön változik (a `ui.c` átalakítás alatt), ebben a komponensben szándékosan nincs bekötve.

## Referenciák

- **pymcuprog** / **pyupdi** (Microchip): UPDI link-réteg + NVMCTRL folyam mintapéldány.
- **jtag2updi**: AVR-en futó UPDI-programozó — single-wire UART + echo-kezelés.
- Datasheet-ek: tinyAVR 0/1/2 és megaAVR0 „UPDI" + „NVMCTRL" fejezetek (regisztercímek, parancsok, kulcsok).
- ATmel/Microchip „AVR UPDI" alkalmazási megjegyzés (frame-formátum, enable-időzítés).
