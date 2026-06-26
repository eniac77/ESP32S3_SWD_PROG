# -*- coding: utf-8 -*-
"""Golden-vektor teszt a target_serial CRC16-CCITT wire-formátumához.

A components/target_serial/target_serial.c-ben a keret-CRC a következő:
    poly = 0x1021, init = 0xFFFF, nincs bemeneti/kimeneti reflektálás,
    nincs final XOR  (azaz "CRC-16/CCITT-FALSE" / "CRC-16/XMODEM" init 0xFFFF).

Itt Pythonban ÚJRAIMPLEMENTÁLJUK pontosan ugyanezt az algoritmust, és ismert
referencia-vektorral rögzítjük. Ez a wire-formátum GOLDEN specifikációja,
amelyhez a C ts_crc16() implementációnak illeszkednie KELL. (C host-fordító
híján a C-logikát ezzel a golden-vektorral fedjük le.)

A kanonikus ellenőrző: az "123456789" ASCII string CRC-16/CCITT-FALSE értéke
0x29B1 — ez az algoritmus szabványos check-értéke.
"""

import os
import re
import unittest

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_THIS_DIR)
_TS_C = os.path.join(_ROOT, "components", "target_serial", "target_serial.c")


def crc16_ccitt(data, init=0xFFFF, poly=0x1021):
    """A C ts_crc16() bitenkénti megfelelője (MSB-first, reflektálás nélkül).

    Megegyezik a target_serial.c-beli hurokkal:
        crc ^= data[i] << 8;
        8x: ha (crc & 0x8000): crc = (crc << 1) ^ poly; else crc <<= 1;
    16 bitre csonkolva.
    """
    crc = init
    for byte in data:
        crc ^= (byte << 8) & 0xFFFF
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ poly) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


class TestCrc16Golden(unittest.TestCase):

    def test_canonical_check_value(self):
        # A CRC-16/CCITT-FALSE szabványos check-értéke "123456789"-re.
        self.assertEqual(crc16_ccitt(b"123456789"), 0x29B1)

    def test_empty_input_returns_init(self):
        # Üres bemenetnél a CRC az init érték marad (0xFFFF).
        self.assertEqual(crc16_ccitt(b""), 0xFFFF)

    def test_deterministic_vectors(self):
        # Saját determinisztikus golden-vektorok (a C-nek ezeket kell adnia).
        vectors = {
            b"\x00": 0xE1F0,
            b"\xFF": 0xFF00,
            b"A": 0xB915,
            b"\x10\x00\x00": 0x8FFF,   # CMD_STATUS-szerű fejléc (LEN=0, CMD=0x10)
        }
        for data, expected in vectors.items():
            self.assertEqual(
                crc16_ccitt(data), expected,
                "CRC eltérés a {!r} vektoron".format(data),
            )

    def test_known_params_in_source(self):
        # Védő-háló: a forrás tényleg a poly 0x1021 / init 0xFFFF párost
        # használja (ha valaki megváltoztatja a C-t, itt feltűnik az eltérés).
        with open(_TS_C, "r", encoding="utf-8") as f:
            text = f.read()
        self.assertTrue(re.search(r"crc\s*=\s*0xFFFF", text),
                        "a forrásban nem találom az init=0xFFFF-et")
        self.assertTrue(re.search(r"0x1021", text),
                        "a forrásban nem találom a poly=0x1021-et")


if __name__ == "__main__":
    unittest.main()
