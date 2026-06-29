# Mérföldkő — első működő SWD-flash valódi hardveren

**Dátum:** 2026-06-29
**Eredmény:** Az ESP32-S3 SWD-programozó **valódi STM32 célt felprogramozott és ellenőrzött**, end-to-end, megbízhatóan és gyorsan.

- **Cél:** STM32F030x8/F05x, **DEV_ID 0x440**, DP IDCODE `0x0bb11477`, RDP0, 64 KB flash.
- **Firmware:** `ECO_Lamella.bin`, **29 288 bájt** @ `0x08000000`.
- **Csatlakozás:** 6-pines fejléc, **nRST nélkül**, tisztán SWD-n (SWCLK=GPIO4, SWDIO=GPIO5, GND, VTARGET). A connect szoftveres reset (SYSRESETREQ) + reset-vektor-catch — ugyanúgy, ahogy az ST-Link is „software reset"-tel megy ezen a csatlakozón.
- **Indítás:** helyi enkóderrel (web UI nem szükséges).
- **Eredmény:** `flash KÉSZ`, read-back verify (memcmp) **átment**.
- **Idő:** **~3,1 s** egy 29 KB-os flashre (erase + program + verify), megbízhatóan.

A SWD/FLM mag platformfüggetlen maradt; minden javítás a `swd_phy` (PHY HAL), `adiv5` (transport), `cortexm_debug` (connect), `prog_session`/`ui` (orchestráció) rétegekben történt — a mag elágazás-mentes.

---

## 1. A bring-up hibalánc (mind szoftveres volt, nem bekötés)

A `swd_phy`/`adiv5` mag fordult és „késznek" tűnt, de HW-n négy, egymás után előkerülő hiba blokkolta a kommunikációt. Diagnosztikához egy boot-idejű PHY-önteszt (`swd_phy_selftest_io()`) készült, ami loopback + tri-state próbával bizonyította, hogy a PHY elektromosan jó.

| # | Tünet | Gyökérok | Javítás | Commit |
|---|---|---|---|---|
| 1 | `ACK=0x7` (csupa-1) minden tranzakción | A `swd_phy_dir(false)` **nem tri-state-elte** a SWDIO-t: a dedic_gpio a GPIO-mátrixon át hajt, a pad OEN-jét a periféria adja, így a `gpio_ll_output_disable` hatástalan → a cél nem tud lehúzni. | `GPIO.func_out_sel_cfg[SWDIO].oen_sel = 1` → az OEN a `GPIO_ENABLE`-ből jön; az ADAT marad a dedic kimenetről. | `c38bbe9` |
| 2 | `ACK=0x0` olvasáson (DPIDR=0) | A `seq_in` a `clk_high()` **után** mintázott; a cél a felfutó élen vált → 1 bit csúszás, az ACK LSB elveszik. | Mintavétel a **low fázisban**, a felfutó él ELŐTT (szimmetrikus a write-tal). | `2f61f70` |
| 3 | `ACK=0x0` **szórványosan íráson** | A SW-DP-nek **trailing idle** kell egy write lezárásához; a kód 0 idle-t tett írás után. | 8 idle ciklus (SWDIO=0) a `swd_write_raw` végén. | `3db81bf` |
| 4 | Glitch után **deszinkronizáció**, nem-helyreálló | Nem volt protokoll-hiba-recovery. | **`dp_resync`**: line reset + JTAG→SWD switch + DPIDR + sticky-törlés + **power-up** + SELECT visszaállítás; minden wrapper retry-zik (`PROTO_RETRY_MAX`). A beragadt-alacsony vonalat **tétlen szünet** oldja (8/16/24 ms), a switch-hammer nem. A block-olvasás `s_resync_count`-tal újraindítja a chunk-ot. A connect-szekvencia 6× újrapróbál. | `25ef8fe`, `04e2bfd`, `313cad1`, `7f1c758`, `8b06821`, `3bc4620`, `28abae8` |

Mellék: a write turnaround-kontenció (ACK→adat TRN előbb meghajtásra váltott) javítva (`1dc3acb`) — bár utóbb kiderült, nem ez volt a fő glitch-ok.

---

## 2. A glitch VALÓDI gyökéroka — WiFi rádió zaja

A fenti recovery átvitte a flash-t a Program fázisig, de **~2% glitch-rátával** (egy futásban 292 re-sync), és időnként nem-helyreállítható `DHCSR=0`. A `dp_resync` flush-fázisában logolt „szabad SWDIO szint" mutatta: glitch előtt 1, után 0 — **a cél (vagy valami) aktívan lehúzta** a vonalat, HALT-olt magon is.

**Felismerés:** a flash idejére leállítva a WiFi rádiót, a `re-sync` szám **292 → 2** esett, és a connect első próbára ment. Tehát a **WiFi RF/táp-zaja csatolt be a bit-bang SWD-be**. Az enkóderes flasheléshez a WiFi nem kell.

- **Fix:** `net_wifi_radio_pause(true/false)` (`esp_wifi_stop/start`); a `prog_session` a **flash ÉS a detect** idejére leállítja a rádiót, a végén vissza. Commit `e70d605`.
- A korábbi recovery innentől csak ritka biztonsági háló (~1-2 esemény/flash).

> Megjegyzés: power-cycle után a 2 MHz-es verify-hiba is eltűnt (10/10 OK) — a sebesség-hangoláskor látott egyszeri verify-eltérés beragadt cél-állapot volt, nem a SWCLK.

---

## 3. Sebesség-optimalizálás — 214 s → 3,1 s (~70×)

Kiindulás (VERBOSE log, ~100 kHz bit-bang): **~214 s** egy 29 KB flashre. Lépcsők:

| Lépés | Mit | Hatás | Commit |
|---|---|---|---|
| Csendes log | A flash idejére a `adiv5/cortexm/flm_runner/swd_phy` tag-ek INFO szintre (a per-tranzakció VERBOSE az idő ~90%-a volt). | ~214 → 22 s | `7f75aea` |
| SWCLK feltolás | Bring-up 300 kHz (megbízható), adatfázis **`freq=0`** (nincs NOP-késleltetés). | 22 → 7 s | `7f75aea`, `8a62df8` |
| PHY gyors út | `seq_out`/`seq_in` 0-késleltetésű ág + nyers egy-utasításos `dedic_gpio_cpu_ll_*` (a nem-inline bundle-wrapper helyett). | (a bit nem volt a fal — lásd lent) | `fb0acdf`, `5345ca1` |
| **OLED-throttle** | A `ui_flash_cb` MINDEN progress-hívásra teljes OLED-frame-et flush-elt (~25 ms I2C); a verify ~115×-ször → **~2,9 s csak kijelző**. Throttle: fázisváltáskor/terminálkor/250 ms-enként. | **7 → 3,1 s** | `8a1e005`, `6242519` |
| Verify blokk | Read-back 64 → 256 B chunk. | kis nyereség | `7bf1655` |

**Tanulság:** a nyers dedic-utasítás (5345ca1) **nem** gyorsított — ez bizonyította, hogy nem a bit-toggle, hanem az **OLED-flush** volt a rejtett szűk keresztmetszet (a progress-callback hívásonkénti teljes frame-rajza).

### Végső fázis-idők (~3,1 s)
- Connect + FLM-loader betöltés + Init: ~0,26 s
- Erase (sector): ~0,7 s — *fizikai (cél)*
- Program (29 KB): ~1,71 s — nagyrészt *fizikai flash-írás* + per-page FLM-hívás
- Verify (29 KB read-back + memcmp): ~0,38 s

---

## 4. Megmaradt lehetőségek (nem blokkoló)

- **Program-fázis nem-flash része:** per-page FLM-hívásnál most mind a 16 magregiszter kiíródik (DCRSR/DCRDR); valójában csak r0/r1/r2 (cím/méret/buffer) változik. Csak a változók írása ~0,2-0,3 s-ot faragna.
- **Multi-család HW-teszt:** eddig csak F030 (0x440) igazolt élesben; más DEV_ID-k (F4/F7/G0/L4…) tesztje hátra.
- **L0/L1 RDP-regiszter címe** még `RDP_REG_NONE` — ellenőrizni.
- A 2 MHz+ SWCLK ezen a bekötésen működött power-cycle után; a `freq=0` (effektíve néhány MHz) a megbízható max — más bekötésnél/célnál újra kell mérni (a verify a háló).

## 5. Kulcs-fájlok
- `components/swd_phy/swd_phy.c` — PHY HAL (tri-state oen_sel, low-fázis minta, gyors út, önteszt).
- `components/adiv5/adiv5.c` — transport (re-sync + retry, trailing idle, turnaround).
- `components/cortexm_debug/cortexm_debug.c` — connect-under-reset (SYSRESETREQ + vektor-catch, 6× retry).
- `components/prog_session/prog_session.c` — orchestráció (WiFi-pause, log-szint, SWCLK-profil).
- `components/ui/ui.c` — `ui_flash_cb` progress-throttle.
- `tools/serial_logger.py` + `ESP32S3_SWD_PROG_log.bat` — soros logger fájlba (fejlesztéshez).
