#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
flm_extract.py — build-time CMSIS .FLM -> C generátor.

Egy vagy több ARM ELF formátumú .FLM fájlból kinyeri:
  - a PrgCode (+ PrgData) szekció bájtjait  -> C `static const uint8_t ..._code[]`,
  - a DevDsc szekció CMSIS `FlashDevice` struktúráját (név, bázis, méret,
    page-méret, törölt érték, timeoutok, szektor-tömb),
  - a belépési pontok offsetjeit a szimbólumtáblából (Init, UnInit, EraseChip,
    EraseSector, ProgramPage, Verify), a kód szekció kezdetéhez viszonyítva.

A kimenet egy újraépíthető (idempotens) `.c` fájl, amely a
`components/flm_blobs/flm_blobs.h` `flm_algo_t` / `flm_sector_t` típusait
használja, és egy `const flm_algo_t * const <tabla>[]` + count párost ad.

Függőség: pyelftools  ->  `pip install pyelftools`

Használat:
    python tools/flm_extract.py FLM... -o out.c [--header out.h] \
        [--table-name g_flm_algos] [--count-name g_flm_algos_count]

A scriptet a CMake `add_custom_command` hívja (lásd flm_blobs/CMakeLists.txt
tervezett bekötését), de önállóan is futtatható (`--help`).
"""

import argparse
import os
import re
import struct
import sys

try:
    from elftools.elf.elffile import ELFFile
except ImportError:
    sys.stderr.write(
        "HIBA: a pyelftools nincs telepítve. Telepítsd: pip install pyelftools\n"
    )
    sys.exit(2)


# ---------------------------------------------------------------------------
# CMSIS FlashDevice konstansok (FlashOS.h)
# ---------------------------------------------------------------------------

# A DevDsc szekció elején ülő FlashDevice struktúra fix része, little-endian.
# Mezők (FlashOS.h `struct FlashDevice`):
#   Vers      : u16   (verzió)
#   DevName   : char[128]
#   DevType   : u16
#   DevAdr    : u32   (flash bázis)
#   szDev     : u32   (teljes flash méret)
#   szPage    : u32   (ProgramPage granularitás)
#   Res       : u32   (_reserved)
#   valEmpty  : u8    (törölt érték, tipikusan 0xFF)
#   <3 byte pad a következő u32 igazításhoz>
#   toProg    : u32   (program timeout, ms)
#   toErase   : u32   (erase timeout, ms)
# Ezután a FlashSectors[]: {szSector(u32), AddrSector(u32)} párok,
# lezárva: 0xFFFFFFFF, 0xFFFFFFFF.

_DEV_NAME_LEN = 128
# A struktúra fejléce a szektor-tömbig (a pyelftools által adott nyers bájtokon).
#  <H   Vers
#  128s DevName
#  H    DevType
#  I    DevAdr
#  I    szDev
#  I    szPage
#  I    Res (_reserved)
#  B    valEmpty
#  3x   pad
#  I    toProg
#  I    toErase
_DEV_HDR_FMT = "<H{}sHIIIIB3xII".format(_DEV_NAME_LEN)
_DEV_HDR_SIZE = struct.calcsize(_DEV_HDR_FMT)

_SECTOR_END = 0xFFFFFFFF

# A belépési pont szimbólumok neve -> a flm_algo_t off_* mezője.
_ENTRY_SYMBOLS = [
    ("Init", "off_init"),
    ("UnInit", "off_uninit"),
    ("EraseChip", "off_erase_chip"),
    ("EraseSector", "off_erase_sector"),
    ("ProgramPage", "off_program_page"),
    ("Verify", "off_verify"),
]


class FlmError(Exception):
    """Érthető, build-leállító hibaüzenet a hibás/hiányos .FLM-ekhez."""


# ---------------------------------------------------------------------------
# ELF segédfüggvények
# ---------------------------------------------------------------------------

def _find_section(elf, names):
    """Visszaadja az első létező szekciót a megadott névlistából (vagy None)."""
    for name in names:
        sec = elf.get_section_by_name(name)
        if sec is not None:
            return sec
    return None


def _read_code_section(elf, path):
    """A PrgCode szekció (cím + bájtok). Hiányában érthető hiba."""
    sec = _find_section(elf, ["PrgCode"])
    if sec is None:
        raise FlmError(
            "'{}': hiányzik a 'PrgCode' szekció — nem érvényes CMSIS .FLM.".format(path)
        )
    return sec["sh_addr"], sec.data()


def _read_data_section(elf):
    """A PrgData szekció bájtjai (lehet üres/None). A loader a kód mögé tölti."""
    sec = _find_section(elf, ["PrgData", "DevData"])
    if sec is None:
        return b""
    return sec.data()


def _read_devdsc(elf, path):
    """A DevDsc szekció nyers bájtjai. Hiányában érthető hiba."""
    sec = _find_section(elf, ["DevDsc"])
    if sec is None:
        raise FlmError(
            "'{}': hiányzik a 'DevDsc' szekció — nem található FlashDevice leíró.".format(path)
        )
    data = sec.data()
    if len(data) < _DEV_HDR_SIZE:
        raise FlmError(
            "'{}': a 'DevDsc' szekció rövidebb ({} bájt), mint a FlashDevice fejléc ({} bájt).".format(
                path, len(data), _DEV_HDR_SIZE
            )
        )
    return data


def _parse_flash_device(devdsc, path):
    """A DevDsc bájtjaiból a FlashDevice struct + szektor-lista kibontása."""
    (
        vers,
        dev_name_raw,
        dev_type,
        dev_adr,
        sz_dev,
        sz_page,
        _reserved,
        val_empty,
        to_prog,
        to_erase,
    ) = struct.unpack_from(_DEV_HDR_FMT, devdsc, 0)

    # A DevName NUL-terminált; latin-1, hogy semmilyen bájt ne dobjon el.
    dev_name = dev_name_raw.split(b"\x00", 1)[0].decode("latin-1").strip()
    if not dev_name:
        dev_name = "FLM"

    # Szektor-tömb a fejléc után, 0xFFFFFFFF,0xFFFFFFFF-ig.
    sectors = []
    off = _DEV_HDR_SIZE
    while off + 8 <= len(devdsc):
        sz_sector, addr_sector = struct.unpack_from("<II", devdsc, off)
        off += 8
        if sz_sector == _SECTOR_END and addr_sector == _SECTOR_END:
            break
        sectors.append((sz_sector, addr_sector))

    if not sectors:
        raise FlmError(
            "'{}': a FlashSectors tömb üres — legalább egy szektor-leíró kell.".format(path)
        )

    return {
        "vers": vers,
        "name": dev_name,
        "dev_type": dev_type,
        "dev_addr": dev_adr,
        "dev_size": sz_dev,
        "page_size": sz_page,
        "erased_val": val_empty,
        "timeout_prog_ms": to_prog,
        "timeout_erase_ms": to_erase,
        "sectors": sectors,
    }


def _collect_entry_offsets(elf, code_addr, path):
    """A belépési pontok offsetje a kód szekció kezdetéhez képest (0 = nincs).

    A Thumb bit (legalsó bit) maszkolva. Hiányzó szimbólum -> 0 offset + figyelm.
    """
    # Szimbólumtábla(ák) összegyűjtése: név -> érték.
    sym_values = {}
    for sec_name in (".symtab", ".dynsym"):
        symtab = elf.get_section_by_name(sec_name)
        if symtab is None:
            continue
        for sym in symtab.iter_symbols():
            if not sym.name:
                continue
            # Az első nem-üres definíció nyer.
            sym_values.setdefault(sym.name, sym["st_value"])

    if not sym_values:
        raise FlmError(
            "'{}': nincs használható szimbólumtábla (.symtab/.dynsym) — "
            "a belépési pontok nem nyerhetők ki.".format(path)
        )

    offsets = {}
    missing = []
    for sym_name, field in _ENTRY_SYMBOLS:
        if sym_name in sym_values:
            # Thumb bit le, offset a kód szekció elejéhez képest.
            addr = sym_values[sym_name] & ~1
            off = addr - code_addr
            if off < 0:
                # Nem a kód szekcióba mutat — kezeljük hiányzóként.
                missing.append(sym_name)
                offsets[field] = 0
            else:
                offsets[field] = off
        else:
            offsets[field] = 0
            # Init/UnInit/ProgramPage tipikusan kötelező; a többi opcionális.
            if sym_name in ("Init", "ProgramPage"):
                missing.append(sym_name)

    if "off_init" in offsets and offsets["off_init"] == 0 and "Init" in [m for m in missing]:
        # Csak figyelmeztetés, nem hiba: hagyjuk a buildet menni.
        sys.stderr.write(
            "FIGYELEM ('{}'): hiányzó kötelezőnek tűnő szimbólum(ok): {}\n".format(
                path, ", ".join(missing)
            )
        )

    return offsets


# ---------------------------------------------------------------------------
# C-azonosító + literál generátorok
# ---------------------------------------------------------------------------

def _c_ident(text):
    """Tetszőleges szövegből érvényes, ütközésmentes C-azonosító-töredék."""
    ident = re.sub(r"[^0-9A-Za-z_]", "_", text)
    if not ident or ident[0].isdigit():
        ident = "_" + ident
    return ident


def _byte_array_lines(data, indent="    ", per_line=12):
    """Bájttömb C-literál sorai (12 bájt/sor, 0xNN formátum)."""
    if not data:
        return [indent + "0x00  /* üres */"]
    lines = []
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append(indent + ", ".join("0x{:02X}".format(b) for b in chunk) + ",")
    # Az utolsó vessző maradhat (C-ben megengedett tömb-inicializálóban).
    return lines


def _c_string(text):
    """Biztonságos C string-literál (idézőjel/backslash escape-elve)."""
    out = []
    for ch in text:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        else:
            out.append("\\x{:02x}".format(ord(ch) & 0xFF))
    return '"' + "".join(out) + '"'


# ---------------------------------------------------------------------------
# Egy .FLM feldolgozása
# ---------------------------------------------------------------------------

def process_flm(path):
    """Egy .FLM -> normalizált dict (név, kód, data, FlashDevice, offsetek)."""
    if not os.path.isfile(path):
        raise FlmError("nem található a fájl: {}".format(path))

    with open(path, "rb") as f:
        try:
            elf = ELFFile(f)
        except Exception as exc:  # pyelftools: ELFError stb.
            raise FlmError("'{}': nem érvényes ELF/.FLM: {}".format(path, exc))

        code_addr, code = _read_code_section(elf, path)
        data = _read_data_section(elf)
        devdsc = _read_devdsc(elf, path)
        dev = _parse_flash_device(devdsc, path)
        offsets = _collect_entry_offsets(elf, code_addr, path)

    # Egyedi C-azonosító az algo nevéből + a fájlnévből (ütközés-elkerülés).
    base = os.path.splitext(os.path.basename(path))[0]
    sym = _c_ident(base)

    return {
        "path": path,
        "sym": sym,
        "dev": dev,
        "code": code,
        "data": data,
        "offsets": offsets,
    }


# ---------------------------------------------------------------------------
# Kimenet összeállítása
# ---------------------------------------------------------------------------

_GEN_HEADER = (
    "/* === GENERÁLT FÁJL — NE SZERKESZD KÉZZEL! ===\n"
    " * Forrás: tools/flm_extract.py (CMSIS .FLM -> C).\n"
    " * Újragenerálható; minden kézi módosítás elveszik a következő buildnél.\n"
    " */\n"
)


def _emit_algo(out, algo):
    """Egy flm_algo_t + a hozzá tartozó kód/szektor tömbök kiírása."""
    sym = algo["sym"]
    dev = algo["dev"]
    off = algo["offsets"]

    out.append("/* ----- {} ({}) ----- */".format(dev["name"], algo["path"]))

    # Kód (PrgCode + PrgData a kód mögé fűzve a loadernek; a prg_data_len jelzi).
    out.append("static const uint8_t {}_code[] = {{".format(sym))
    out.extend(_byte_array_lines(algo["code"]))
    out.append("};")
    out.append("")

    # Szektor-tömb.
    out.append("static const flm_sector_t {}_sectors[] = {{".format(sym))
    for sz, addr in dev["sectors"]:
        out.append("    {{ .size = 0x{:08X}u, .addr = 0x{:08X}u }},".format(sz, addr))
    out.append("};")
    out.append("")

    # Maga az algo struktúra (a flm_blobs.h mezőneveivel pontosan egyezően).
    out.append("static const flm_algo_t {}_algo = {{".format(sym))
    out.append("    .name             = {},".format(_c_string(dev["name"])))
    out.append("    .dev_addr         = 0x{:08X}u,".format(dev["dev_addr"]))
    out.append("    .dev_size         = 0x{:08X}u,".format(dev["dev_size"]))
    out.append("    .page_size        = 0x{:08X}u,".format(dev["page_size"]))
    out.append("    .erased_val       = 0x{:02X}u,".format(dev["erased_val"]))
    out.append("    .timeout_prog_ms  = {}u,".format(dev["timeout_prog_ms"]))
    out.append("    .timeout_erase_ms = {}u,".format(dev["timeout_erase_ms"]))
    out.append("    .sectors          = {}_sectors,".format(sym))
    out.append("    .sector_count     = {}u,".format(len(dev["sectors"])))
    out.append("    .prg_code         = {}_code,".format(sym))
    out.append("    .prg_code_len     = {}u,".format(len(algo["code"])))
    out.append("    .prg_data_len     = {}u,".format(len(algo["data"])))
    out.append("    .off_init         = 0x{:08X}u,".format(off["off_init"]))
    out.append("    .off_uninit       = 0x{:08X}u,".format(off["off_uninit"]))
    out.append("    .off_erase_chip   = 0x{:08X}u,".format(off["off_erase_chip"]))
    out.append("    .off_erase_sector = 0x{:08X}u,".format(off["off_erase_sector"]))
    out.append("    .off_program_page = 0x{:08X}u,".format(off["off_program_page"]))
    out.append("    .off_verify       = 0x{:08X}u,".format(off["off_verify"]))
    out.append("};")
    out.append("")


def generate_c(algos, table_name, count_name, header_name=None):
    """A teljes .c forrás szövege string-ként."""
    out = []
    out.append(_GEN_HEADER)
    if header_name:
        out.append('#include "{}"'.format(os.path.basename(header_name)))
    else:
        out.append('#include "flm_blobs.h"')
    out.append("")

    for algo in algos:
        _emit_algo(out, algo)

    # A pointer-tábla + count.
    out.append("/* A beépített FLM algoritmusok táblája. */")
    out.append("const flm_algo_t * const {}[] = {{".format(table_name))
    for algo in algos:
        out.append("    &{}_algo,".format(algo["sym"]))
    out.append("};")
    out.append("")
    out.append("const size_t {} = sizeof({}) / sizeof({}[0]);".format(
        count_name, table_name, table_name))
    out.append("")
    return "\n".join(out)


def generate_h(table_name, count_name):
    """Opcionális fejléc a generált tábla extern deklarációival."""
    out = []
    out.append(_GEN_HEADER)
    out.append("#pragma once")
    out.append('#include "flm_blobs.h"')
    out.append("")
    out.append('#ifdef __cplusplus')
    out.append('extern "C" {')
    out.append('#endif')
    out.append("")
    out.append("extern const flm_algo_t * const {}[];".format(table_name))
    out.append("extern const size_t {};".format(count_name))
    out.append("")
    out.append('#ifdef __cplusplus')
    out.append('}')
    out.append('#endif')
    out.append("")
    return "\n".join(out)


def _write_if_changed(path, text):
    """Idempotens írás: csak akkor ír, ha változott a tartalom."""
    if os.path.isfile(path):
        try:
            with open(path, "r", encoding="utf-8") as f:
                if f.read() == text:
                    return False
        except OSError:
            pass
    d = os.path.dirname(os.path.abspath(path))
    if d and not os.path.isdir(d):
        os.makedirs(d)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return True


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(
        description="CMSIS .FLM (ARM ELF) -> C tömb generátor (flm_algo_t).",
    )
    parser.add_argument(
        "flm", nargs="+", metavar="FLM",
        help="egy vagy több .FLM bemeneti fájl",
    )
    parser.add_argument(
        "-o", "--output", required=True, metavar="OUT.C",
        help="kimeneti .c fájl útvonala",
    )
    parser.add_argument(
        "--header", metavar="OUT.H", default=None,
        help="opcionális kimeneti .h (extern deklarációkkal)",
    )
    parser.add_argument(
        "--table-name", default="g_flm_algos",
        help="a generált tábla neve (alap: g_flm_algos)",
    )
    parser.add_argument(
        "--count-name", default="g_flm_algos_count",
        help="a count konstans neve (alap: g_flm_algos_count)",
    )
    args = parser.parse_args(argv)

    try:
        algos = [process_flm(p) for p in args.flm]
    except FlmError as exc:
        sys.stderr.write("HIBA: {}\n".format(exc))
        return 1

    c_text = generate_c(algos, args.table_name, args.count_name, args.header)
    changed = _write_if_changed(args.output, c_text)
    sys.stderr.write(
        "{} {} ({} algoritmus)\n".format(
            "Írva:" if changed else "Változatlan:", args.output, len(algos))
    )

    if args.header:
        h_text = generate_h(args.table_name, args.count_name)
        _write_if_changed(args.header, h_text)
        sys.stderr.write("Fejléc: {}\n".format(args.header))

    return 0


if __name__ == "__main__":
    sys.exit(main())
