# Host-oldali tesztek (`tests/`)

PC-n (host) futó, **hardver nélkül** ellenőrizhető logikát fedő tesztek, tiszta
Python `unittest`-tel (NINCS pytest, csak az stdlib). Céljuk a projekt
legkockázatosabb, ELF/struct/wire-formátum jellegű logikájának lefedése.

## Mit fednek le

| Teszt | Mit igazol |
|-------|-----------|
| `test_flm_extract.py` | A `tools/flm_extract.py` ST `.stldr` parszere VALÓDI loadereken: StorageInfo (DeviceStartAddress, DeviceSize, PageSize, EraseValue), a belépési pontok abszolút RAM-címei (Thumb-bit maszkolva, 0x2000xxxx), `load_addr`, ST ABI, `success_ret=1`. Negatív eset: hiányzó / nem-ELF fájl -> `FlmError`. |
| `test_flm_targetdb_consistency.py` | A generált `flm_generated.c` `.dev_id` halmaza és a `target_db.c` DEV_ID-jai konzisztensek: minden generált loaderhez van `target_db` sor (különben a `select_flm` sosem találná meg), és nincs duplikált DEV_ID a generált táblában. (A csak-`target_db`-ben szereplő DEV_ID-k megengedettek — info-ként kiírva.) |
| `test_crc16.py` | A `target_serial.c` keret-CRC16 (CCITT, poly `0x1021`, init `0xFFFF`, reflektálás/final-XOR nélkül) **golden-vektorai**. Kanonikus check: `"123456789"` -> `0x29B1`, plusz determinisztikus saját vektorok. |

## Miért golden-vektor / konzisztencia?

A C-logika (`ts_crc16`, `flm_algo_t` tábla) host C-fordító híján nem futtatható
közvetlenül a CI-ben. Ezért:

- a **CRC16** wire-formátumát Pythonban újraimplementáljuk és ismert
  referencia-értékkel rögzítjük — ez a **specifikáció**, amelyhez a C-nek
  illeszkednie kell (a `test_crc16.py` egyúttal ellenőrzi, hogy a forrásban
  tényleg a `0x1021` / `0xFFFF` paraméterek szerepelnek);
- a **generált tábla** helyességét a forrás `.stldr`-ekből újraszámolva
  (a tényleges generátorral) és a `target_db`-vel összevetve **konzisztencia**
  szinten fedjük.

## Futtatás

Az ESP-IDF Python-jával kell futtatni (abban van `pyelftools`):

```powershell
# Windows / PowerShell
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1
```

```bash
# bash (Git Bash / WSL)
bash tests/run_tests.sh
```

Vagy közvetlenül:

```
C:\Users\jenei\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe -m unittest discover -s tests -v
```

## Megjegyzések

- Ha egy `.stldr` (pl. `0x431.stldr`, `0x413.stldr`) nincs telepítve a gépen
  (`C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\FlashLoader\`),
  az adott teszt **SKIP**-el, nem bukik.
- A tesztek NEM buildelnek `idf.py`-vel és nem nyúlnak a célhardverhez.
