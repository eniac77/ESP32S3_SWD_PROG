#!/usr/bin/env python3
"""Egyszerű soros logger: COM-port -> fájl, soronként flush-elve.

A SWD-flash fejlesztéshez: a flash-t a felhasználó indítja (enkóder/web UI),
ez a script csak naplóz egy fájlba, amit külső eszköz (pl. AI-asszisztens)
közvetlenül olvashat — nem kell a monitorból másolgatni.

Használat:
    python serial_logger.py [PORT] [OUTFILE] [BAUD]
Alapértékek: COM18, <repo>/reference/monitorlog.txt, 115200
Kilépés: Ctrl + C
"""
import os
import sys
import time

try:
    import serial  # pyserial (az ESP-IDF python env-ben elérhető)
except ImportError:
    sys.stderr.write("HIBA: nincs pyserial. Futtasd ESP-IDF python kornyezetbol.\n")
    sys.exit(1)

_here = os.path.dirname(os.path.abspath(__file__))
_default_out = os.path.normpath(os.path.join(_here, "..", "reference", "monitorlog.txt"))

port = sys.argv[1] if len(sys.argv) > 1 else "COM18"
outfile = sys.argv[2] if len(sys.argv) > 2 else _default_out
baud = int(sys.argv[3]) if len(sys.argv) > 3 else 115200

# Megnyitás újrapróbálással (ha a port épp foglalt egy monitortól).
ser = None
for _ in range(20):
    try:
        ser = serial.Serial(port, baud, timeout=1)
        break
    except Exception as e:
        sys.stderr.write(f"port nyitas varakozas ({port}): {e}\n")
        time.sleep(0.5)
if ser is None:
    sys.stderr.write(f"NEM sikerult megnyitni: {port}\n")
    sys.exit(1)

sys.stderr.write(f"LOGGER FUT: {port} @ {baud} -> {outfile}\n")
sys.stderr.write("Kilepes: Ctrl + C\n")
try:
    with open(outfile, "w", encoding="utf-8", errors="replace") as f:
        while True:
            data = ser.readline()
            if data:
                f.write(data.decode("utf-8", "replace"))
                f.flush()
except KeyboardInterrupt:
    sys.stderr.write("\nLOGGER LEALLITVA.\n")
finally:
    ser.close()
