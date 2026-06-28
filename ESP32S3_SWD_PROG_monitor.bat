@echo off
rem ==========================================================================
rem  ESP32S3_SWD_PROG - soros MONITOR (feltoltes nelkul).
rem  A build-beli ELF-fel dekodolja a cimeket. Kilepes: Ctrl + ]
rem  Itt latod a teljes naplot: boot-banner, SWD onteszt (DEV_ID/flash/RDP),
rem  es a reszletes SWD/FLM/flash logot.
rem  Hasznalat:  monitor.bat        -> a lenti PORT
rem              monitor.bat COM7   -> a megadott port
rem ==========================================================================
setlocal

rem --- Allitsd at ide a sajat COM portodat ---
set "PORT=COM10"
if not "%~1"=="" set "PORT=%~1"
if "%PORT%"=="" set /p PORT=COM port pl. COM10:

set "PY=%USERPROFILE%\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe"
set "BUILD=%~dp0build"

if not exist "%PY%" goto no_py
if not exist "%BUILD%\esp32s3_swd_prog.elf" goto no_elf

cd /d "%BUILD%"

rem Az xtensa toolchaint a PATH-ra tesszuk, hogy a monitor dekodolni tudja a
rem backtrace cimeket (kulonben nem talalja az addr2line-t -> WinError).
set "TC="
for /d %%d in ("%USERPROFILE%\.espressif\tools\xtensa-esp-elf\*") do set "TC=%%d\xtensa-esp-elf\bin"
if defined TC set "PATH=%TC%;%PATH%"

echo.
echo Monitor a %PORT% porton - kilepes: Ctrl + zarojel-be ]
echo.
"%PY%" -m esp_idf_monitor --target esp32s3 --toolchain-prefix xtensa-esp32s3-elf- -p %PORT% esp32s3_swd_prog.elf
exit /b %ERRORLEVEL%

:no_py
echo HIBA: nem talalom az ESP-IDF python kornyezetet:
echo   %PY%
pause
exit /b 1

:no_elf
echo HIBA: nincs ELF. Futtass elobb: build.bat
echo   %BUILD%
pause
exit /b 1
