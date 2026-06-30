# AVR PDI (XMEGA) — protokoll- és bring-up referencia

**Állapot:** implementálva (`components/avr_pdi/`), **HW-n MÉG NEM IGAZOLT**.
Ez a doksi a `avr_pdi.c`-ben bekötött protokollt írja le, és a valódi célon való
bring-up-hoz ad checklistet. Társdoksik: [AVR_UPDI.md](AVR_UPDI.md),
[MILESTONE_SWD_HW.md](MILESTONE_SWD_HW.md).

## 1. Mire való, milyen célok

PDI = **Program and Debug Interface**: az **XMEGA** család (ATxmega...) két-vezetékes,
szinkron program/debug interfésze. Nem PDI: a klasszikus ATtiny/ATmega (SPI-ISP →
`avr_isp`), a modern tinyAVR/megaAVR/Dx (UPDI → `avr_updi`).

## 2. Fizikai bekötés (ezen a készüléken)

| Jel | ESP láb | Megjegyzés |
|---|---|---|
| **PDI_CLK** | **GPIO21** (a header RESET-vonala) | a programozó adja az órajelet; a target **RESET lába = PDI_CLK** |
| **PDI_DATA** | **GPIO7** (a header MISO-ja) | kétirányú adat; turnaround (mint SWDIO) |
| GND | GND | közös föld |

- A PDI_CLK fizikailag a target RESET lába — ezért a meglévő RESET-drót pont oda megy.
- **PDI_DATA kétirányú**: bit-bang turnaround (`data_dir()` ki/be váltás), idle = magas, belső felhúzással.
- **Pad-ütközés:** a GPIO7/GPIO21 megegyezik az ISP MISO/RESET és az UPDI lábbal → **PDI-t és UPDI-t NE kapcsold be egyszerre** (ugyanaz a GPIO7). Mindhárom interfész session-szinten kizárja egymást.
- SCK/MOSI ESP-lábak PDI módban nem használtak.

## 3. PHY — bit-bang, 12-bites keret

- **Keret:** start(0) + **8 adatbit LSB-first** + **páros paritás** + **2 stop**(1), idle magas. (Ugyanaz a frame-formátum, mint UPDI-nál, de itt **külön órajel** van.)
- **Órajel:** mi adjuk a PDI_CLK-t. Adatküldéskor a bit a **felfutó élen** mintázódik a célnál; olvasáskor a magas szakaszon mintázunk. Default **~150 kHz** (`PDI_HALF_US=3`), szinkron → clock-stretch nélkül is biztos.
- **Turnaround:** olvasás előtt `data_dir(false)` (PDI_DATA bemenet), a cél a guard-time idle bitek után küldi a start-bitet; a `pdi_rx_byte()` keresi a start-bitet (max 64 idle clock), majd beolvassa a 8 adat + paritás + 2 stop bitet.

## 4. Engedélyezés + NVM

```
data_dir(true); PDI_DATA=1;
>=16 (kódban 24) CLK ciklus, DATA magasban  // RESET-funkció kikapcs, PDI BE
tx_idle(16);
STCS PDI_CTRL = guard-time (0x07 = 2 idle)
LDCS PDI_STATUS                              // válaszol-e a link?
KEY 0xE0 + 8 kulcsbájt (NVM key)             // 0x1289AB45CDD888FF, LSB-first
LDCS PDI_STATUS -> NVMEN (bit1=0x02)?        // NVM engedélyezve
```

PDI control/status regiszterek (LDCS/STCS): `0x00 STATUS` (NVMEN=bit1), `0x01 RESET`
(0x59 apply / 0x00 release), `0x02 CTRL` (guard time).

## 5. XMEGA NVM-vezérlő

Data-space regiszterek (base **0x01C0**): `ADDR0..2 (0x01C0..)`, `DATA0 (0x01C4)`,
`CMD (0x01CA)`, `CTRLA (0x01CB, CMDEX=bit0)`, `STATUS (0x01CF, NVMBUSY=bit7, FBUSY=bit6)`.

Parancsok (CMD): NOP=0x00, **CHIP_ERASE=0x40**, **READ_NVM=0x43**,
**LOAD_FLASH_BUFFER=0x23**, ERASE_FLASH_BUFFER=0x26, **ERASE_WRITE_FLASH_PAGE=0x2F**.

**Signature:** `MCU.DEVID0..2` a data-space `0x0090..0x0092`-n → `LDS` közvetlenül
(nem kell NVM-parancs).

**Flash leképzése a PDI cím-térben:** `PDI_FLASH_BASE = 0x00800000` (XMEGA app flash).

Lap-programozás (kódban):
```
CHIP_ERASE -> CMDEX -> NVMBUSY poll          // app szekció törlése
for each page:
    CMD = LOAD_FLASH_BUFFER
    for b in page: STS (flash_base + addr + b) = data[b]   // page buffer
    CMD = ERASE_WRITE_FLASH_PAGE
    STS (flash_base + page_addr) = 0          // a flash-iras triggereli a parancsot
    NVMBUSY poll
verify: CMD = READ_NVM; LDS (flash_base + i)  // visszaolvasas + osszevetes
```

## 6. Bring-up checklist (valódi célon)

A SWD/UPDI-fegyelem itt is áll (csendben, lépésenként):

1. **PHY/turnaround**: a `data_dir()` valóban tri-state-eli-e a PDI_DATA-t (a SWDIO-tanulság: a dedic/matrix OEN trükk a SWD-nél kellett; itt sima `gpio_set_direction` megy, mert nem dedic_gpio — HW-n ellenőrizni a felengedést).
2. **Enable**: a >=16 CLK után a `LDCS PDI_STATUS` ad-e értelmes választ (nem csupa-1/0).
3. **NVM key**: a `STATUS` NVMEN bitje álljon be a KEY után — ha nem, a kulcs-bájtsorrend vagy a bekötés a gyanús.
4. **Signature**: `LDS 0x0090..0x0092` adja a vártat (pl. ATxmega128A1: 1E 97 4C). 0xFF/0x00 → áram/baud/bekötés.
5. **Paritás/framing**: ha sok az `RX paritashiba`/`framing`, a fél-órajel (`PDI_HALF_US`) vagy a mintavételi él hangolandó.
6. **Egy lap** programozása + visszaolvasás, mielőtt a teljes képet futtatnád.

## 7. Hátralévő / bizonytalan (HW-validáció tisztázza)

- A **flash-bázis** (`0x00800000`) és az **ERASE_WRITE_FLASH_PAGE trigger** módja (a kód a flash-címre írással triggerel; lehet, hogy `NVM_ADDR` + `CMDEX` kell helyette) — avrdude XMEGA-forrással egyeztetendő.
- A **signature** olvasás `MCU.DEVID`-ből vs. a production signature row (NVM READ_NVM) — mindkettő járható, a DEVID az egyszerűbb.
- **Lapméretek/flash-méretek** a `PDI_TABLE`-ben (HW-n ellenőrizendő).
- **EEPROM/fuse/lockbit** kezelés — még nincs.
- **Sebesség**: a per-bájt `STS` lassú; `REPEAT` + `ST *(ptr++)` streaminggel gyorsítható.
- **UI-menü**: külön változik (a `ui.c` átalakítás alatt), ebben a komponensben nincs bekötve.

## Referenciák

- **avrdude** (`xmega.c` / PDI driver): az XMEGA NVM-folyam (CMD/CMDEX, LOAD_FLASH_BUFFER, ERASE_WRITE) kanonikus mintája.
- **Atmel AVR1612** „PDI programming driver" alkalmazási megjegyzés: enable-időzítés, frame-formátum, utasításkészlet.
- XMEGA AU manual: NVM controller regiszterek, parancsok, memóriatérkép.
