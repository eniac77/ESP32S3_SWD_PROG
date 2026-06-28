@echo off
rem ==========================================================================
rem  ESP32S3_SWD_PROG - CSAK A STORAGE (LittleFS): 0x310000 storage.bin
rem  A build mappabol flashel. Az app NEM frissul.
rem  A storage.bin a data/ fabol (web-UI: /lfs/www + /lfs/fw,/lfs/cfg) generalodik
rem  a build soran (littlefs_create_partition_image). Ezzel csak a webfelulet/
rem  konyvtarstruktura frissitheto ujraflashelt app nelkul.
rem  FIGYELEM: felulirja a teljes storage particiot (a feltoltott fw/cfg torlodik)!
rem  Hasznalat:  storage_flash.bat        -> a lenti PORT
rem              storage_flash.bat COM7   -> a megadott port
rem ==========================================================================
setlocal

rem --- Allitsd at ide a sajat COM portodat ---
set "PORT=COM18"
if not "%~1"=="" set "PORT=%~1"
if "%PORT%"=="" set /p PORT=COM port pl. COM10:

set "PY=%USERPROFILE%\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe"
set "BUILD=%~dp0build"

if not exist "%PY%" goto no_py
if not exist "%BUILD%\storage.bin" goto no_build

cd /d "%BUILD%"
echo.
echo STORAGE feltoltes a %PORT% portra, 460800 baud...
echo.
"%PY%" -m esptool --chip esp32s3 -p %PORT% -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x310000 storage.bin

set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" goto flash_err

echo KESZ - a storage feltoltve. Monitor inditasa - kilepes: Ctrl + zarojel-be ]
echo.
rem Az xtensa toolchaint a PATH-ra tesszuk a backtrace-dekodolashoz.
set "TC="
for /d %%d in ("%USERPROFILE%\.espressif\tools\xtensa-esp-elf\*") do set "TC=%%d\xtensa-esp-elf\bin"
if defined TC set "PATH=%TC%;%PATH%"
"%PY%" -m esp_idf_monitor --target esp32s3 --toolchain-prefix xtensa-esp32s3-elf- -p %PORT% esp32s3_swd_prog.elf
exit /b 0

:flash_err
echo HIBA - esptool kilepesi kod: %RC%
pause
exit /b %RC%

:no_py
echo HIBA: nem talalom az ESP-IDF python kornyezetet:
echo   %PY%
pause
exit /b 1

:no_build
echo HIBA: nincs storage.bin a build mappaban. Futtass elobb: build.bat
echo   %BUILD%
pause
exit /b 1
