# -*- coding: utf-8 -*-
"""Host-oldali teszt a tools/flm_extract.py ST `.stldr` parszeréhez.

Ezek a tesztek VALÓDI STM32CubeProgrammer `.stldr` loadereken futnak (ha a gépen
elérhetők). Igazolják, hogy a build-time generátor helyesen olvassa ki a
StorageInfo-t és a belépési pontok abszolút RAM-címeit — ez a legkockázatosabb,
hardver nélkül is ellenőrizhető logika (ELF-parszolás, struct-layout).

Ha egy `.stldr` hiányzik a gépről, az adott teszt SKIP-el (nem bukik).
"""

import os
import sys
import unittest

# A projekt gyökere és a tools/ a sys.path-hez, hogy a flm_extract importálható
# legyen modulként (a publikus process_stldr() függvényét hívjuk).
_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_THIS_DIR)
_TOOLS = os.path.join(_ROOT, "tools")
if _TOOLS not in sys.path:
    sys.path.insert(0, _TOOLS)

import flm_extract  # noqa: E402

# Az ST flash-loaderek helye (DEV_ID szerint nevezve: 0x431.stldr stb.).
_STLDR_DIR = (
    r"C:\Program Files\STMicroelectronics\STM32Cube"
    r"\STM32CubeProgrammer\bin\FlashLoader"
)


def _stldr_path(dev_id_hex):
    """Egy .stldr abszolút útja a DEV_ID hex-rövid neve alapján (pl. '431')."""
    return os.path.join(_STLDR_DIR, "0x{}.stldr".format(dev_id_hex))


def _have(dev_id_hex):
    return os.path.isfile(_stldr_path(dev_id_hex))


class TestStldrParse431(unittest.TestCase):
    """STM32F411 (DEV_ID 0x431, 512 KB flash)."""

    @unittest.skipUnless(_have("431"), "0x431.stldr nincs telepítve")
    def test_storage_info_and_entrypoints(self):
        algo = flm_extract.process_stldr(_stldr_path("431"))
        dev = algo["dev"]

        # --- StorageInfo eszköz-leíró ---
        self.assertEqual(dev["dev_addr"], 0x08000000)
        self.assertEqual(dev["dev_size"], 512 * 1024)
        # PageSize legyen értelmes (nem 0, kettő-hatvány, F4-en tipikusan 16 KB).
        self.assertGreater(dev["page_size"], 0)
        self.assertEqual(dev["page_size"] & (dev["page_size"] - 1), 0,
                         "a PageSize nem kettő-hatvány")
        self.assertEqual(dev["page_size"], 16384)
        self.assertEqual(dev["erased_val"], 0xFF)

        # --- ABI / siker-visszatérés ---
        self.assertEqual(algo["abi"], "FLM_ABI_ST")
        self.assertEqual(algo["success_ret"], 1)

        # --- A loader RAM-ra töltési címe ---
        self.assertEqual(algo["load_addr"], 0x20000004)

        # --- DEV_ID a fájlnévből ---
        self.assertEqual(algo["dev_id"], 0x431)

        # --- Belépési pontok: abszolút RAM-címek a 0x2000xxxx tartományban,
        #     a Thumb-bit (alsó bit) lemaszkolva -> páros címek. ---
        offsets = algo["offsets"]
        for field in ("off_init", "off_st_write", "off_st_sector_erase"):
            addr = offsets.get(field, 0)
            self.assertNotEqual(addr, 0, "{} hiányzik".format(field))
            self.assertEqual(addr & 0xFFFF0000, 0x20000000,
                             "{} nem a 0x2000xxxx tartományban: 0x{:08X}"
                             .format(field, addr))
            self.assertEqual(addr & 1, 0,
                             "{} alsó (Thumb) bitje nincs maszkolva".format(field))

        # MassErase és Verify F4-en jelen van; ha van, szintén páros 0x2000xxxx.
        for field in ("off_st_mass_erase", "off_verify"):
            addr = offsets.get(field, 0)
            if addr:
                self.assertEqual(addr & 0xFFFF0000, 0x20000000)
                self.assertEqual(addr & 1, 0)


class TestStldrParse413(unittest.TestCase):
    """STM32F4 1 MB változat (DEV_ID 0x413)."""

    @unittest.skipUnless(_have("413"), "0x413.stldr nincs telepítve")
    def test_storage_info(self):
        algo = flm_extract.process_stldr(_stldr_path("413"))
        dev = algo["dev"]
        self.assertEqual(dev["dev_addr"], 0x08000000)
        self.assertEqual(dev["dev_size"], 1024 * 1024)
        self.assertEqual(algo["abi"], "FLM_ABI_ST")
        self.assertEqual(algo["load_addr"], 0x20000004)


class TestStldrNegative(unittest.TestCase):
    """Negatív eset: a parser a saját hibatípusát (FlmError) dobja."""

    def test_missing_file_raises_flmerror(self):
        bogus = os.path.join(_THIS_DIR, "nincs_ilyen_fajl_12345.stldr")
        self.assertFalse(os.path.exists(bogus))
        with self.assertRaises(flm_extract.FlmError):
            flm_extract.process_stldr(bogus)

    def test_not_an_elf_raises_flmerror(self):
        # Egy érvénytelen (nem-ELF) fájlt írunk a scratch helyett a tests/ alá,
        # majd töröljük — a parser FlmError-t kell dobjon rá.
        path = os.path.join(_THIS_DIR, "_bad_loader.tmp")
        with open(path, "wb") as f:
            f.write(b"NEM ELF TARTALOM, csak szemet\x00\x01\x02")
        try:
            with self.assertRaises(flm_extract.FlmError):
                flm_extract.process_stldr(path)
        finally:
            os.remove(path)


if __name__ == "__main__":
    unittest.main()
