# Host-oldali tesztek futtatása az ESP-IDF Python-nal (pyelftools-szal).
# Használat:  powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1
$ErrorActionPreference = "Stop"

# Az IDF Python (pyelftools elérhető benne).
$Py = "C:\Users\jenei\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe"
if (-not (Test-Path $Py)) {
    Write-Error "Nem találom az IDF Python-t: $Py"
    exit 1
}

# A projekt gyökere = a tests/ szülője.
$Root = Split-Path -Parent $PSScriptRoot

Push-Location $Root
try {
    & $Py -m unittest discover -s tests -v
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
