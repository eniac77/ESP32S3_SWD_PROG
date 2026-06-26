# -*- coding: utf-8 -*-
"""Konzisztencia-teszt a generált FLM-tábla és a target_db közt.

Kockázat: a futásidőben a select_flm() a cél DEV_ID-jával keres loadert a
generált táblában, a target_db pedig a DEV_ID -> család/flash-méret leképezést
adja. Ha egy generált loaderhez NINCS target_db sor, akkor azt a kiválasztó
soha nem találja meg (és fordítva: target_db sor generált loader nélkül csak
"nincs beépített algoritmus" — ez megengedett).

Ez a teszt forrásfájlokat parszol reguláris kifejezéssel (nem build), így
hardver és C-fordító nélkül is fut.
"""

import os
import re
import unittest

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_THIS_DIR)

_FLM_GENERATED = os.path.join(_ROOT, "components", "flm_blobs", "flm_generated.c")
_TARGET_DB = os.path.join(_ROOT, "components", "target_db", "target_db.c")

# A generált struct dev_id mezője:  ".dev_id           = 0x0431u,"
_RE_GEN_DEVID = re.compile(r"\.dev_id\s*=\s*0[xX]([0-9A-Fa-f]+)\s*u?", re.MULTILINE)

# A target_db tábla-sorai:  "{ STM32_FAM_F4, "STM32F411", 0x431, 0xE004... }"
# Az első hex literál a string név UTÁN a DEV_ID.
_RE_DB_ROW = re.compile(
    r"\{\s*STM32_FAM_\w+\s*,\s*\"[^\"]*\"\s*,\s*0[xX]([0-9A-Fa-f]+)",
    re.MULTILINE,
)


def _read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


class TestFlmTargetDbConsistency(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.gen_text = _read(_FLM_GENERATED)
        cls.db_text = _read(_TARGET_DB)
        cls.gen_ids = [int(m, 16) for m in _RE_GEN_DEVID.findall(cls.gen_text)]
        cls.db_ids = [int(m, 16) for m in _RE_DB_ROW.findall(cls.db_text)]

    def test_files_parsed_nonempty(self):
        # Védő-háló: ha a regex nem talál semmit, az maga is hiba lenne.
        self.assertGreater(len(self.gen_ids), 0, "nincs .dev_id a flm_generated.c-ben")
        self.assertGreater(len(self.db_ids), 0, "nincs sor a target_db.c-ben")

    def test_no_duplicate_dev_id_in_generated(self):
        seen = {}
        for d in self.gen_ids:
            seen[d] = seen.get(d, 0) + 1
        dups = sorted(d for d, c in seen.items() if c > 1)
        self.assertEqual(
            dups, [],
            "duplikált DEV_ID a generált táblában: "
            + ", ".join("0x{:X}".format(d) for d in dups),
        )

    def test_every_generated_id_present_in_target_db(self):
        gen = set(self.gen_ids)
        db = set(self.db_ids)
        missing = sorted(gen - db)
        # Infó: target_db-ben van olyan, amihez nincs generált loader (megengedett).
        db_only = sorted(db - gen)
        if db_only:
            print(
                "\n[INFO] target_db DEV_ID generált loader nélkül (megengedett): "
                + ", ".join("0x{:X}".format(d) for d in db_only)
            )
        self.assertEqual(
            missing, [],
            "generált loader DEV_ID-k, amelyekhez NINCS target_db sor "
            "(a select_flm sosem találná meg): "
            + ", ".join("0x{:X}".format(d) for d in missing),
        )


if __name__ == "__main__":
    unittest.main()
