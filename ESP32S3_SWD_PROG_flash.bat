@echo off
rem ==========================================================================
rem  ESP32S3_SWD_PROG - CSAK A PROGRAM (app): 0x10000 esp32s3_swd_prog.bin
rem  (factory app, nincs OTA). A build mappabol flashel. A storage NEM frissul.
rem  Hasznalat:  flash.bat               -> a lenti PORT, 921600 baud
rem              flash.bat COM7          -> a megadott port
rem              flash.bat COM7 1500000  -> port + baud (ha a soros hid birja)
rem  Elotte legyen lefordulva:  build.bat  (vagy idf.py build)
rem ==========================================================================
setlocal

rem --- Allitsd at ide a sajat COM portodat ---
set "PORT=COM10"
if not "%~1"=="" set "PORT=%~1"
if "%PORT%"=="" set /p PORT=COM port pl. COM10:

rem --- Flash baud (felulirhato a 2. parameterrel). 921600 szinte minden hidnal
rem     megy; 1500000 / 2000000 gyorsabb, ha a soros hid (pl. CH343) birja.
rem     (ESP32-S3 native USB-Serial-JTAG eseten a baud lenyegtelen.) ---
set "BAUD=921600"
if not "%~2"=="" set "BAUD=%~2"

set "PY=%USERPROFILE%\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe"
set "BUILD=%~dp0build"

if not exist "%PY%" goto no_py
if not exist "%BUILD%\esp32s3_swd_prog.bin" goto no_build

cd /d "%BUILD%"
echo.
echo APP feltoltes a %PORT% portra, %BAUD% baud...
echo.
"%PY%" -m esptool --chip esp32s3 -p %PORT% -b %BAUD% --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x10000 esp32s3_swd_prog.bin

set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" goto flash_err

echo KESZ - az app feltoltve. Monitor inditasa - kilepes: Ctrl + zarojel-be ]
echo.
rem Az xtensa toolchaint a PATH-ra tesszuk, hogy a monitor dekodolni tudja a
rem backtrace cimeket (kulonben nem talalja az addr2line-t -> WinError).
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
echo HIBA: nincs app-bin. Futtass elobb: build.bat
echo   %BUILD%
pause
exit /b 1
