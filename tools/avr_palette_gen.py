#!/usr/bin/env python3
"""avrdude.conf(.in) -> a teljes 8-bites AVR paletta C-tabla sorai.

Az avr_isp / avr_updi / avr_pdi komponensek DEV-tablait generalja az avrdude.conf
(vagy .in) forrasbol, interfesz szerint (PM_ISP / PM_UPDI / PM_PDI / PM_TPI)
kategorizalva, a parent-orokles feloldasaval.

Hasznalat:
    python tools/avr_palette_gen.py <avrdude.conf.in> [--out DIR]

Ha --out meg van adva, DIR/{isp,updi,pdi}_table.txt fajlokba ir; kulonben stdout.
A tablak beillesztheto formaban keszulnek (a komponens .c array-body helyere).

Forras: https://raw.githubusercontent.com/avrdudes/avrdude/main/src/avrdude.conf.in
A nvm_ver-t a UPDI flash-bazis donti: 0x800000 -> v2 (Ex: v3), egyebkent v0.
"""
import re, sys, os, argparse, collections


def parse(text):
    lines = text.splitlines()
    blocks, cur = [], None
    for ln in lines:
        if re.match(r'^part\b', ln):
            cur = [ln]
        elif cur is not None:
            cur.append(ln)
            if re.match(r'^;\s*$', ln):
                blocks.append(cur); cur = None

    def pblock(b):
        d = dict(parent=None, id=None, desc=None, pm=None, sig=None,
                 fsz=None, fpg=None, foff=None)
        m = re.match(r'^part\s+parent\s+"([^"]+)"', b[0])
        if m: d["parent"] = m.group(1)
        in_flash = False
        for ln in b:
            for k, rx in (("id", r'\s*id\s*=\s*"([^"]+)"'),
                          ("desc", r'\s*desc\s*=\s*"([^"]+)"')):
                m = re.match(rx, ln)
                if m: d[k] = m.group(1)
            m = re.match(r'\s*prog_modes\s*=\s*(.+?);', ln)
            if m: d["pm"] = m.group(1).strip()
            m = re.match(r'\s*signature\s*=\s*(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)', ln)
            if m: d["sig"] = tuple(int(m.group(i), 16) for i in (1, 2, 3))
            if re.match(r'\s*memory\s+"flash"', ln): in_flash = True; continue
            if in_flash:
                if re.match(r'\s*;\s*$', ln): in_flash = False; continue
                for k, rx in (("fsz", r'\s*size\s*=\s*(0x[0-9a-fA-F]+|\d+)'),
                              ("fpg", r'\s*page_size\s*=\s*(0x[0-9a-fA-F]+|\d+)'),
                              ("foff", r'\s*offset\s*=\s*(0x[0-9a-fA-F]+|\d+)')):
                    m = re.match(rx, ln)
                    if m: d[k] = int(m.group(1), 0)
        return d

    parts, order = {}, []
    for b in blocks:
        d = pblock(b)
        if d["id"]: parts[d["id"]] = d; order.append(d["id"])
    return parts, order


def resolve(parts, pid, field, seen=None):
    seen = seen or set()
    if pid in seen or pid not in parts: return None
    seen.add(pid)
    v = parts[pid][field]
    if v is not None: return v
    par = parts[pid]["parent"]
    return resolve(parts, par, field, seen) if par else None


def updi_ver(desc, foff):
    base = foff if foff is not None else 0x8000
    if base == 0x800000:
        if re.match(r'AVR\d+(EA|EB)', desc.upper()): return (3, True, 0x800000)
        return (2, True, 0x800000)
    return (0, False, base)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("conf")
    ap.add_argument("--out")
    a = ap.parse_args()
    text = open(a.conf, encoding="utf-8", errors="replace").read()
    parts, order = parse(text)

    rows = []
    for pid in order:
        if pid.startswith("."): continue
        desc = parts[pid]["desc"] or pid
        if desc.startswith("."): continue
        sig = resolve(parts, pid, "sig")
        if not sig: continue
        rows.append(dict(desc=desc, sig=sig,
                         pm=resolve(parts, pid, "pm") or "",
                         fsz=resolve(parts, pid, "fsz"),
                         fpg=resolve(parts, pid, "fpg"),
                         foff=resolve(parts, pid, "foff")))

    byif, seen = collections.defaultdict(list), collections.defaultdict(set)
    for r in rows:
        pm = r["pm"]
        i = ("UPDI" if "PM_UPDI" in pm else "PDI" if "PM_PDI" in pm
             else "ISP" if ("PM_ISP" in pm or "PM_SPM" in pm)
             else "TPI" if "PM_TPI" in pm else "OTHER")
        if r["sig"] in seen[i]: continue
        seen[i].add(r["sig"]); byif[i].append(r)

    hs = lambda s: "0x%02X, 0x%02X, 0x%02X" % s

    def isp_lines():
        out = []
        for r in sorted(byif["ISP"], key=lambda x: x["sig"]):
            if not re.match(r'AT(tiny|mega|90)', r["desc"]): continue
            if not (r["fpg"] or 0) > 0: continue
            out.append('    { { %s }, "%s", %d, %d },' % (hs(r["sig"]), r["desc"], r["fsz"] or 0, r["fpg"]))
        return out

    def updi_lines():
        out = []
        for r in sorted(byif["UPDI"], key=lambda x: x["sig"]):
            ver, a24, base = updi_ver(r["desc"], r["foff"])
            out.append('    { { %s }, "%s", 0x%X, %d, 0x%X, %d, %s },' % (
                hs(r["sig"]), r["desc"], r["fsz"] or 0, r["fpg"] or 0, base, ver, "true" if a24 else "false"))
        return out

    def pdi_lines():
        out = []
        for r in sorted(byif["PDI"], key=lambda x: x["sig"]):
            out.append('    { { %s }, "%s", 0x%X, %d },' % (hs(r["sig"]), r["desc"], r["fsz"] or 0, r["fpg"] or 0))
        return out

    def tpi_lines():
        # reduced-core tiny: { sig, name, flash, write-size (word=2 v. page) }
        out = []
        for r in sorted(byif["TPI"], key=lambda x: x["sig"]):
            out.append('    { { %s }, "%s", 0x%X, %d },' % (hs(r["sig"]), r["desc"], r["fsz"] or 0, r["fpg"] or 0))
        return out

    tables = {"isp": isp_lines(), "updi": updi_lines(), "pdi": pdi_lines(), "tpi": tpi_lines()}
    sys.stderr.write("ISP=%d UPDI=%d PDI=%d TPI=%d\n" % (
        len(tables["isp"]), len(tables["updi"]), len(tables["pdi"]), len(tables["tpi"])))

    if a.out:
        os.makedirs(a.out, exist_ok=True)
        for k, v in tables.items():
            with open(os.path.join(a.out, k + "_table.txt"), "w", newline="\n") as f:
                f.write("\n".join(v) + "\n")
    else:
        for k, v in tables.items():
            print("=== %s (%d) ===" % (k.upper(), len(v)))
            print("\n".join(v))
            print()


if __name__ == "__main__":
    main()
