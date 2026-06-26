#!/usr/bin/env bash
# Host-oldali tesztek futtatása az ESP-IDF Python-nal (pyelftools-szal).
# Használat:  bash tests/run_tests.sh
set -euo pipefail

# Az IDF Python (pyelftools elérhető benne).
PY="C:/Users/jenei/.espressif/python_env/idf5.5_py3.13_env/Scripts/python.exe"
if [ ! -f "$PY" ]; then
    echo "Nem találom az IDF Python-t: $PY" >&2
    exit 1
fi

# A projekt gyökere = a tests/ szülője.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"

cd "$ROOT"
exec "$PY" -m unittest discover -s tests -v
