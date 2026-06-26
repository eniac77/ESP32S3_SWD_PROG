#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
flm_extract.py — build-time flash-loader -> C generátor.

Elsődlegesen a STM32CubeProgrammer `.stldr` (ST loader-ABI) belső flash
loadereit dolgozza fel, és a `components/flm_blobs/flm_blobs.h` `flm_algo_t`
típusát használó, újraépíthető C táblát generál belőlük.

A `.stldr` egy IAR-épített ARM ELF. Felépítése (ellenőrizve több eszközön):
  - Kód-szekció (neve eszközönként eltér: 'P1 ro', 'P1', 'P1-P2 ro' …) a cél
    RAM abszolút címére (tipikusan 0x20000004) töltve. Ebben ülnek a belépési
    pont függvények.
  - StorageInfo szekció (neve 'P2 ro', 'P2', 'P3' …, gyakran sh_addr=0) az
    eszköz-leíró OBJECT-tel.
  - .symtab abszolút RAM-címekkel (Thumb-bit az alsó biten):
        Init(void), Write(addr,size,buf), SectorErase(start,end),
        MassErase(void) [opcionális], Verify(...). Siker = visszatérés 1.
  - StorageInfo OBJECT szimbólum (méret ~200), layout:
        char     DeviceName[100];
        uint32_t DeviceType;
        uint32_t DeviceStartAddress;
        uint32_t DeviceSize;
        uint32_t PageSize;
        uint8_t  EraseValue;  /* + 3 bájt pad */
        struct { uint32_t SectorNum; uint32_t SectorSize; } sectors[]; // {0,0}-ig

A kód-szekciót és a StorageInfo-t NEM név alapján keressük (eszközönként más),
hanem a szimbólumok `st_shndx` szekció-indexe alapján — ez robusztus.

A klasszikus CMSIS `.FLM` ág megmaradt (DevDsc/PrgCode, 0=siker), de a projekt
jelenleg ST `.stldr`-eket használ.

Függőség: pyelftools  ->  `pip install pyelftools`

Használat:
    python tools/flm_extract.py 0x431.stldr 0x413.stldr -o out.c --header out.h
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
# Konstansok
# ---------------------------------------------------------------------------

# ST StorageInfo layout offsetek.
_ST_NAME_LEN = 100
_ST_FIXED_FMT = "<IIIIB"          # DeviceType, Start, Size, PageSize, EraseValue
_ST_SECTORS_OFF = 120             # a {num,size} csoportok kezdete
_ST_MIN_SIZE = _ST_SECTORS_OFF + 8

# ST belépési pont szimbólumok -> a flm_algo_t off_* mezője.
# Init/Write/SectorErase kötelező. MassErase opcionális (L0/L1 nem ad).
# Verify opcionális: a flm_runner ST-ágon visszaolvasással (adiv5 + memcmp)
# verifikál, így off_verify hiánya nem gond (pl. L011/L152MD+/L4Rx nem ad Verify-t).
_ST_ENTRY_SYMBOLS = [
    ("Init", "off_init", True),
    ("Write", "off_st_write", True),
    ("SectorErase", "off_st_sector_erase", True),
    ("MassErase", "off_st_mass_erase", False),
    ("Verify", "off_verify", False),
]

# ST timeoutok (ms) — fix, a StorageInfo nem tartalmazza.
_ST_TIMEOUT_PROG_MS = 5000
_ST_TIMEOUT_ERASE_MS = 30000
_ST_SUCCESS_RET = 1

# --- CMSIS .FLM (örökölt ág) ---
_DEV_NAME_LEN = 128
_DEV_HDR_FMT = "<H{}sHIIIIB3xII".format(_DEV_NAME_LEN)
_DEV_HDR_SIZE = struct.calcsize(_DEV_HDR_FMT)
_SECTOR_END = 0xFFFFFFFF
_CMSIS_ENTRY_SYMBOLS = [
    ("Init", "off_init"),
    ("UnInit", "off_uninit"),
    ("EraseChip", "off_erase_chip"),
    ("EraseSector", "off_erase_sector"),
    ("ProgramPage", "off_program_page"),
    ("Verify", "off_verify"),
]


class FlmError(Exception):
    """Érthető, build-leállító hibaüzenet a hibás/hiányos loaderekhez."""


# ---------------------------------------------------------------------------
# ELF segédfüggvények
# ---------------------------------------------------------------------------

def _collect_symbols(elf, path):
    """A .symtab/.dynsym szimbólumai: név -> sym objektum (első definíció nyer)."""
    syms = {}
    for sec_name in (".symtab", ".dynsym"):
        st = elf.get_section_by_name(sec_name)
        if st is None:
            continue
        for sym in st.iter_symbols():
            if sym.name:
                syms.setdefault(sym.name, sym)
    if not syms:
        raise FlmError(
            "'{}': nincs használható szimbólumtábla (.symtab/.dynsym).".format(path)
        )
    return syms


def _section_of_symbol(elf, sym, path, what):
    """A szimbólum szekciója a st_shndx alapján (abszolút szekció-index)."""
    shndx = sym["st_shndx"]
    if shndx in ("SHN_UNDEF", "SHN_ABS", "SHN_COMMON"):
        raise FlmError(
            "'{}': a(z) {} szimbólum nem egy konkrét szekcióban van "
            "(st_shndx={}).".format(path, what, shndx)
        )
    sec = elf.get_section(shndx)
    if sec is None:
        raise FlmError(
            "'{}': a(z) {} szimbólum szekciója nem található "
            "(st_shndx={}).".format(path, what, shndx)
        )
    return sec


# ---------------------------------------------------------------------------
# ST .stldr feldolgozása
# ---------------------------------------------------------------------------

def _parse_storage_info(data, path):
    """A StorageInfo nyers bájtjaiból az eszköz-leíró + szektor-lista."""
    if len(data) < _ST_MIN_SIZE:
        raise FlmError(
            "'{}': a StorageInfo túl rövid ({} bájt < {}).".format(
                path, len(data), _ST_MIN_SIZE)
        )

    name = data[0:_ST_NAME_LEN].split(b"\x00", 1)[0].decode("latin-1").strip()
    (dev_type, dev_addr, dev_size, page_size, erase_val) = struct.unpack_from(
        _ST_FIXED_FMT, data, _ST_NAME_LEN
    )

    # Szektor-csoportok: {SectorNum, SectorSize} a {0,0} őrértékig.
    # Kibontva flm_sector_t {size, addr} listára (addr = eszköz elejéhez relatív).
    sectors = []
    off = _ST_SECTORS_OFF
    rel = 0
    while off + 8 <= len(data):
        num, ssize = struct.unpack_from("<II", data, off)
        off += 8
        if num == 0 and ssize == 0:
            break
        for _ in range(num):
            sectors.append((ssize, rel))
            rel += ssize

    if dev_size == 0:
        raise FlmError("'{}': a StorageInfo DeviceSize=0 — gyanús leíró.".format(path))

    return {
        "name": name or "STLDR",
        "dev_type": dev_type,
        "dev_addr": dev_addr,
        "dev_size": dev_size,
        "page_size": page_size,
        "erased_val": erase_val,
        "sectors": sectors,
    }


def _dev_id_from_name(path):
    """DEV_ID a fájlnévből: '0x431.stldr' -> 0x431. Csak ha egyértelmű."""
    base = os.path.splitext(os.path.basename(path))[0]
    m = re.match(r"^0[xX]([0-9A-Fa-f]+)", base)
    if m:
        try:
            return int(m.group(1), 16)
        except ValueError:
            return 0
    return 0


def process_stldr(path, dev_id_override=None):
    """Egy .stldr -> normalizált algo-dict (ST ABI)."""
    if not os.path.isfile(path):
        raise FlmError("nem található a fájl: {}".format(path))

    with open(path, "rb") as f:
        try:
            elf = ELFFile(f)
        except Exception as exc:
            raise FlmError("'{}': nem érvényes ELF/.stldr: {}".format(path, exc))

        syms = _collect_symbols(elf, path)

        # Kötelező belépési pontok jelenléte.
        for sym_name, _field, required in _ST_ENTRY_SYMBOLS:
            if required and sym_name not in syms:
                raise FlmError(
                    "'{}': hiányzó kötelező szimbólum: {} — nem ST .stldr?".format(
                        path, sym_name)
                )
        if "StorageInfo" not in syms:
            raise FlmError(
                "'{}': hiányzik a 'StorageInfo' szimbólum — nem ST .stldr?".format(path)
            )

        # A kód-szekció = az Init szimbólum szekciója (név eszközönként eltér).
        code_sec = _section_of_symbol(elf, syms["Init"], path, "Init")
        code = code_sec.data()
        load_addr = code_sec["sh_addr"]
        if not code:
            raise FlmError("'{}': a kód-szekció ('{}') üres.".format(path, code_sec.name))

        # A StorageInfo a saját (gyakran külön, sh_addr=0) szekciójában.
        si_sym = syms["StorageInfo"]
        si_sec = _section_of_symbol(elf, si_sym, path, "StorageInfo")
        si_off = si_sym["st_value"] - si_sec["sh_addr"]
        si_size = si_sym["st_size"] or _ST_MIN_SIZE
        si_data = si_sec.data()[si_off:si_off + si_size]
        dev = _parse_storage_info(si_data, path)

        # A belépési pontok ABSZOLÚT címei (Thumb-bit maszkolva). 0 = nincs.
        offsets = {field: 0 for _n, field, _r in _ST_ENTRY_SYMBOLS}
        for sym_name, field, _required in _ST_ENTRY_SYMBOLS:
            if sym_name in syms:
                offsets[field] = syms[sym_name]["st_value"] & ~1

    dev_id = dev_id_override if dev_id_override is not None else _dev_id_from_name(path)

    sym = _c_ident(os.path.splitext(os.path.basename(path))[0])

    return {
        "abi": "FLM_ABI_ST",
        "path": path,
        "sym": sym,
        "dev": dev,
        "dev_id": dev_id,
        "code": code,
        "data": b"",                 # ST: data_len = 0
        "load_addr": load_addr,
        "success_ret": _ST_SUCCESS_RET,
        "timeout_prog_ms": _ST_TIMEOUT_PROG_MS,
        "timeout_erase_ms": _ST_TIMEOUT_ERASE_MS,
        "offsets": offsets,
    }


# ---------------------------------------------------------------------------
# CMSIS .FLM feldolgozása (örökölt ág — megtartva)
# ---------------------------------------------------------------------------

def _find_section(elf, names):
    for name in names:
        sec = elf.get_section_by_name(name)
        if sec is not None:
            return sec
    return None


def _parse_flash_device(devdsc, path):
    (vers, dev_name_raw, dev_type, dev_adr, sz_dev, sz_page,
     _reserved, val_empty, to_prog, to_erase) = struct.unpack_from(
        _DEV_HDR_FMT, devdsc, 0)
    dev_name = dev_name_raw.split(b"\x00", 1)[0].decode("latin-1").strip() or "FLM"
    sectors = []
    off = _DEV_HDR_SIZE
    while off + 8 <= len(devdsc):
        sz_sector, addr_sector = struct.unpack_from("<II", devdsc, off)
        off += 8
        if sz_sector == _SECTOR_END and addr_sector == _SECTOR_END:
            break
        sectors.append((sz_sector, addr_sector))
    if not sectors:
        raise FlmError("'{}': a FlashSectors tömb üres.".format(path))
    return {
        "name": dev_name, "dev_type": dev_type, "dev_addr": dev_adr,
        "dev_size": sz_dev, "page_size": sz_page, "erased_val": val_empty,
        "timeout_prog_ms": to_prog, "timeout_erase_ms": to_erase,
        "sectors": sectors,
    }


def process_flm(path, dev_id_override=None):
    """Egy CMSIS .FLM -> normalizált algo-dict (CMSIS ABI)."""
    if not os.path.isfile(path):
        raise FlmError("nem található a fájl: {}".format(path))
    with open(path, "rb") as f:
        try:
            elf = ELFFile(f)
        except Exception as exc:
            raise FlmError("'{}': nem érvényes ELF/.FLM: {}".format(path, exc))

        code_sec = _find_section(elf, ["PrgCode"])
        if code_sec is None:
            raise FlmError("'{}': hiányzik a 'PrgCode' szekció.".format(path))
        code_addr = code_sec["sh_addr"]
        code = code_sec.data()

        data_sec = _find_section(elf, ["PrgData", "DevData"])
        data = data_sec.data() if data_sec is not None else b""

        devdsc_sec = _find_section(elf, ["DevDsc"])
        if devdsc_sec is None:
            raise FlmError("'{}': hiányzik a 'DevDsc' szekció.".format(path))
        devdsc = devdsc_sec.data()
        if len(devdsc) < _DEV_HDR_SIZE:
            raise FlmError("'{}': a 'DevDsc' túl rövid.".format(path))
        dev = _parse_flash_device(devdsc, path)

        syms = _collect_symbols(elf, path)
        offsets = {field: 0 for _n, field in _CMSIS_ENTRY_SYMBOLS}
        for sym_name, field in _CMSIS_ENTRY_SYMBOLS:
            if sym_name in syms:
                addr = syms[sym_name]["st_value"] & ~1
                off = addr - code_addr
                offsets[field] = off if off >= 0 else 0

    sym = _c_ident(os.path.splitext(os.path.basename(path))[0])
    return {
        "abi": "FLM_ABI_CMSIS",
        "path": path,
        "sym": sym,
        "dev": dev,
        "dev_id": dev_id_override or 0,
        "code": code,
        "data": data,
        "load_addr": code_addr,
        "success_ret": 0,
        "timeout_prog_ms": dev["timeout_prog_ms"],
        "timeout_erase_ms": dev["timeout_erase_ms"],
        "offsets": offsets,
    }


def process_input(path, dev_id_override=None):
    """A kiterjesztés alapján ST (.stldr) vagy CMSIS (.FLM) ágra irányít."""
    ext = os.path.splitext(path)[1].lower()
    if ext == ".stldr":
        return process_stldr(path, dev_id_override)
    if ext == ".flm":
        return process_flm(path, dev_id_override)
    # Ismeretlen kiterjesztés: próbáljuk ST-ként (a projekt ezt használja).
    try:
        return process_stldr(path, dev_id_override)
    except FlmError:
        return process_flm(path, dev_id_override)


# ---------------------------------------------------------------------------
# C-azonosító + literál generátorok
# ---------------------------------------------------------------------------

def _c_ident(text):
    ident = re.sub(r"[^0-9A-Za-z_]", "_", text)
    if not ident or ident[0].isdigit():
        ident = "_" + ident
    return ident


def _byte_array_lines(data, indent="    ", per_line=12):
    if not data:
        return [indent + "0x00  /* üres */"]
    lines = []
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append(indent + ", ".join("0x{:02X}".format(b) for b in chunk) + ",")
    return lines


def _c_string(text):
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
# Kimenet összeállítása
# ---------------------------------------------------------------------------

_GEN_HEADER = (
    "/* === GENERÁLT FÁJL — NE SZERKESZD KÉZZEL! ===\n"
    " * Forrás: tools/flm_extract.py (STM32CubeProgrammer .stldr / CMSIS .FLM -> C).\n"
    " * Újragenerálható; minden kézi módosítás elveszik a következő generálásnál.\n"
    " * A nyers .stldr fájlok ST-proprietary tartalom, ezért nem kerülnek a repóba.\n"
    " */\n"
)


def _emit_algo(out, algo):
    """Egy flm_algo_t + a hozzá tartozó kód/szektor tömbök kiírása."""
    sym = algo["sym"]
    dev = algo["dev"]
    off = algo["offsets"]
    is_st = algo["abi"] == "FLM_ABI_ST"

    out.append("/* ----- {} (DEV_ID 0x{:X}, {}) ----- */".format(
        dev["name"], algo["dev_id"], os.path.basename(algo["path"])))

    # Kód-blob.
    out.append("static const uint8_t {}_code[] = {{".format(sym))
    out.extend(_byte_array_lines(algo["code"]))
    out.append("};")
    out.append("")

    # Szektor-tömb (ha van).
    sectors = dev["sectors"]
    if sectors:
        out.append("static const flm_sector_t {}_sectors[] = {{".format(sym))
        for sz, addr in sectors:
            out.append("    {{ .size = 0x{:08X}u, .addr = 0x{:08X}u }},".format(sz, addr))
        out.append("};")
        out.append("")

    # Az algo struktúra (a flm_blobs.h mezőneveivel pontosan egyezően).
    out.append("static const flm_algo_t {}_algo = {{".format(sym))
    out.append("    .name             = {},".format(_c_string(dev["name"])))
    out.append("    .dev_id           = 0x{:04X}u,".format(algo["dev_id"]))
    out.append("    .abi              = {},".format(algo["abi"]))
    out.append("    .success_ret      = {},".format(algo["success_ret"]))
    out.append("    .dev_addr         = 0x{:08X}u,".format(dev["dev_addr"]))
    out.append("    .dev_size         = 0x{:08X}u,".format(dev["dev_size"]))
    out.append("    .page_size        = 0x{:08X}u,".format(dev["page_size"]))
    out.append("    .erased_val       = 0x{:02X}u,".format(dev["erased_val"]))
    out.append("    .timeout_prog_ms  = {}u,".format(algo["timeout_prog_ms"]))
    out.append("    .timeout_erase_ms = {}u,".format(algo["timeout_erase_ms"]))
    if sectors:
        out.append("    .sectors          = {}_sectors,".format(sym))
        out.append("    .sector_count     = {}u,".format(len(sectors)))
    else:
        out.append("    .sectors          = NULL,")
        out.append("    .sector_count     = 0u,")
    out.append("    .load_addr        = 0x{:08X}u,".format(algo["load_addr"]))
    out.append("    .code             = {}_code,".format(sym))
    out.append("    .code_len         = {}u,".format(len(algo["code"])))
    out.append("    .data_len         = {}u,".format(len(algo["data"])))
    out.append("    .off_init         = 0x{:08X}u,".format(off.get("off_init", 0)))
    out.append("    .off_uninit       = 0x{:08X}u,".format(off.get("off_uninit", 0)))
    out.append("    .off_erase_chip   = 0x{:08X}u,".format(off.get("off_erase_chip", 0)))
    out.append("    .off_erase_sector = 0x{:08X}u,".format(off.get("off_erase_sector", 0)))
    out.append("    .off_program_page = 0x{:08X}u,".format(off.get("off_program_page", 0)))
    out.append("    .off_verify       = 0x{:08X}u,".format(off.get("off_verify", 0)))
    out.append("    .off_st_write        = 0x{:08X}u,".format(off.get("off_st_write", 0)))
    out.append("    .off_st_sector_erase = 0x{:08X}u,".format(off.get("off_st_sector_erase", 0)))
    out.append("    .off_st_mass_erase   = 0x{:08X}u,".format(off.get("off_st_mass_erase", 0)))
    out.append("};")
    out.append("")


def generate_c(algos, table_name, count_name, header_name=None):
    out = []
    out.append(_GEN_HEADER)
    if header_name:
        out.append('#include "{}"'.format(os.path.basename(header_name)))
    else:
        out.append('#include "flm_blobs.h"')
    out.append("")

    for algo in algos:
        _emit_algo(out, algo)

    out.append("/* A beépített flash-loader algoritmusok táblája. */")
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
        description="STM32 .stldr / CMSIS .FLM (ARM ELF) -> C tábla generátor "
                    "(flm_algo_t).",
    )
    parser.add_argument(
        "inputs", nargs="+", metavar="LOADER",
        help="egy vagy több .stldr (ST) vagy .FLM (CMSIS) bemeneti fájl",
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
    parser.add_argument(
        "--dev-id", default=None,
        help="DEV_ID felülírása (egyetlen bemenetnél, pl. 0x431); "
             "alapból a fájlnévből.",
    )
    args = parser.parse_args(argv)

    dev_id_override = None
    if args.dev_id is not None:
        try:
            dev_id_override = int(args.dev_id, 0)
        except ValueError:
            sys.stderr.write("HIBA: érvénytelen --dev-id: {}\n".format(args.dev_id))
            return 1

    try:
        algos = [process_input(p, dev_id_override) for p in args.inputs]
    except FlmError as exc:
        sys.stderr.write("HIBA: {}\n".format(exc))
        return 1

    c_text = generate_c(algos, args.table_name, args.count_name, args.header)
    changed = _write_if_changed(args.output, c_text)

    total_code = sum(len(a["code"]) for a in algos)
    sys.stderr.write(
        "{} {} ({} algoritmus, {} bájt össz. kód)\n".format(
            "Írva:" if changed else "Változatlan:", args.output, len(algos), total_code)
    )
    for a in algos:
        sys.stderr.write(
            "  - {:14} DEV_ID=0x{:X} abi={} code={} bájt load=0x{:08X}\n".format(
                a["dev"]["name"], a["dev_id"], a["abi"], len(a["code"]), a["load_addr"])
        )

    if args.header:
        h_text = generate_h(args.table_name, args.count_name)
        _write_if_changed(args.header, h_text)
        sys.stderr.write("Fejléc: {}\n".format(args.header))

    return 0


if __name__ == "__main__":
    sys.exit(main())
