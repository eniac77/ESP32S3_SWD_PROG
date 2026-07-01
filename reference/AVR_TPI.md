# AVR TPI (reduced-core ATtiny) — protokoll- és bring-up referencia

**Állapot:** implementálva (`components/avr_tpi/`), **HW-n MÉG NEM IGAZOLT**, de a
konstansok az **avrdude** (`src/tpi.h`, `src/avr.c`) + az Atmel **AVR918** (doc8373)
app note-hoz **hitelesítve**. Társdoksik: [AVR_PDI.md](AVR_PDI.md), [AVR_UPDI.md](AVR_UPDI.md).

## 1. Mire való, milyen célok

TPI = **Tiny Programming Interface**: a **reduced-core ATtiny** (ATtiny4/5/9/10/20/40)
és a hozzájuk hasonló ATtiny102/104 programozó/debug interfésze. A `avr_tpi.c`
`TPI_TABLE`-je mind a **8 részt** fedi (`avrdude.conf`-ból). Ezek a legkisebb 8-bites
AVR-ek (0,5–4 KB flash) — a klasszikus ISP-t NEM támogatják.

## 2. Fizikai bekötés (ezen a készüléken)

Három vezeték kell (a meglévő header SCK/MISO/RESET lábai):

| Jel | ESP láb | Szerep |
|---|---|---|
| **TPICLK** | **GPIO15** (SCK) | órajel (a programozó adja) |
| **TPIDATA** | **GPIO7** (MISO) | kétirányú adat (turnaround) |
| **RESET** | **GPIO21** | a cél RESET lába — a TPI alatt **végig alacsony** |

- A RESET alacsonyra húzása kapcsolja be a TPI PHY-t (nincs 12 V, az HVSP más).
- MOSI (GPIO16) nem használt. A GPIO7/15/21 megegyezik az ISP/PDI/UPDI lábakkal → egyszerre csak egy AVR-interfész.

## 3. PHY — bit-bang, 12-bites keret (mint a PDI)

- **Keret:** start(0) + **8 adatbit LSB-first** + **páros paritás** + **2 stop**(1), idle magas. **NINCS SYNC-bájt**.
- **Órajel:** mi adjuk a TPICLK-t; adat a **lefutó élen** vált, a cél a **felfutó élen** mintáz. Default ~150 kHz, szinkron → clock-stretch nélkül is jó.
- **Turnaround/guard:** a cél a válasz előtt **guard-idle biteket** szúr be (default 128, min 2). A `TPIPCR`-be írt kis guard (`0x07`) leszorítja; a `tpi_rx_byte()` a start-bitet keresi (max 64 idle clock). A programozó TX előtt ≥1 idle bitet ad (a `tpi_send` 2 idle-t).

## 4. Engedélyezés + NVM

```
RESET = 0 (alacsony), TPIDATA = 1, >=16 (kódban 32) TPICLK   // TPI PHY BE
SSTCS TPIPCR = 0x07                    // guard time leszorítás
SLDCS TPIIR  -> 0x80 ?                  // TPI ident (link él)
SKEY 0xE0 + 8 kulcsbájt                 // 0x1289AB45CDD888FF, FORDÍTVA: FF 88 D8 CD 45 AB 89 12
SLDCS TPISR -> NVMEN (bit1) ?           // NVM engedélyezve
```

CS-regiszterek (SLDCS/SSTCS): `TPISR=0x00` (NVMEN=bit1), `TPIPCR=0x02` (guard), `TPIIR=0x0F` (ident 0x80).

## 5. Utasításkészlet + NVM

Opkódok (`tpi.h`): `SLD=0x20`, `SLD+=0x24`, `SST=0x60`, `SST+=0x64`, `SSTPR|0=0x68`/`|1=0x69`,
`SIN=0x10|SIO`, `SOUT=0x90|SIO`, `SLDCS=0x80|idx`, `SSTCS=0xC0|idx`, `SKEY=0xE0`.
`SIO_ADDR(x) = ((x&0x30)<<1)|(x&0x0F)` → NVMCSR(0x32)→0x62, NVMCMD(0x33)→0x63.

NVM-vezérlő (I/O): `NVMCSR=0x32` (NVMBSY=bit7), `NVMCMD=0x33`. Parancsok:
NO_OP=0x00, **CHIP_ERASE=0x10**, SECTION_ERASE=0x14, **WORD_WRITE=0x1D** (tiny20=DWORD, tiny40=CODE — ugyanaz a 0x1D, de több szó/trigger).

**Flash a TPI data-térben `0x4000`-nál; signature-sor `0x3FC0`-nál** (SSTPR + SLD+).

Programozás (a **magas-bájt tárolása triggerel**):
```
chip erase: SSTPR (0x4000|1) ; SOUT NVMCMD, CHIP_ERASE ; SST 0xFF ; poll NVMBSY
word write: SOUT NVMCMD, WORD_WRITE ; SSTPR addr ; SST+ low ; SST+ high (trigger) ; poll NVMBSY
  - többszavas (tiny20=2, tiny40=4): a szavak közt 1 idle karakter, a blokk utolsó high-bájtja triggerel
verify:     SSTPR 0x4000 ; SLD+ végig + összevetés
```

## 6. Bring-up checklist (valódi célon)

1. **TPI ident**: `SLDCS TPIIR` = **0x80** az enable után → a link él (RESET low + 16 CLK jó).
2. **Turnaround/guard**: ha sok `RX paritashiba`/`nincs start-bit`, a `TPIPCR` guard-ot növeld (0x07→kisebb GT-érték = több idle) vagy a `TPI_HALF_US`-t.
3. **NVMEN**: `TPISR` bit1 álljon be a SKEY után — ha nem, a kulcs-sorrend (FORDÍTOTT!) vagy a bekötés a gyanús.
4. **Signature**: `SSTPR 0x3FC0` + 3× `SLD+` adja a vártat (pl. ATtiny10: 1E 90 03).
5. **Egy szó** írása + visszaolvasás, mielőtt a teljes képet futtatnád.
6. **block_words** (tiny20=2, tiny40=4): a többszavas trigger-viselkedést HW-n ellenőrizni (a tiny4/5/9/10/102/104 = 1 szó a biztos rész).

## 7. Hátralévő / bizonytalan

- **block_words** a tiny20/40-hez (DWORD/CODE_WRITE): a szavak száma/trigger HW-n ellenőrizendő; a klasszikus 6 rész (WORD_WRITE=1) a biztos.
- **SECTION_ERASE** (fuse/lockbit/EEPROM) — még nincs; a fuse-ok TPI-n írhatók, de a flash-hez nem kell.
- **Sebesség**: a per-szó SST elég gyors a kis flashekhez; nincs streaming-igény.
- **UI-menü**: külön változik (a `ui.c` átalakítás alatt), itt nincs bekötve.

## Referenciák

- **avrdude** `src/tpi.h` (opkódok, kulcs, CS/NVM regiszterek), `src/avr.c` (`avr_tpi_program_enable`, `avr_tpi_chip_erase`, `avr_tpi_poll_nvmbsy`).
- **Atmel AVR918** (doc8373) „Using the Atmel Tiny Programming Interface (TPI)": PHY, enable-szekvencia, NVM-folyam.
- **avrdude.conf**: signature/flash-geometria, `.reduced_core_tiny` memória-offszetek (flash 0x4000, signature 0x3FC0).
