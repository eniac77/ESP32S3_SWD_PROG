@echo off
rem ==========================================================================
rem  ESP32S3_SWD_PROG - soros LOGGER fajlba (feltoltes nelkul).
rem  A soros kimenetet a reference\monitorlog.txt-be irja (soronkent flush).
rem  A flash-t TE inditod (enkoder/web UI) - ez csak naploz, hogy az
rem  AI-asszisztens kozvetlenul olvashassa a logot (nem kell masolgatni).
rem  Kilepes: Ctrl + C
rem  Hasznalat:  log.bat            -> a lenti PORT, reference\monitorlog.txt
rem              log.bat COM7       -> a megadott port
rem              log.bat COM7 my.txt-> sajat kimeneti fajl
rem  Megj.: egyszerre csak EGY program foghatja a portot (logger VAGY monitor).
rem ==========================================================================
setlocal

rem --- Allitsd at ide a sajat COM portodat ---
set "PORT=COM18"
if not "%~1"=="" set "PORT=%~1"
if "%PORT%"=="" set /p PORT=COM port pl. COM10:

set "OUT=%~dp0reference\monitorlog.txt"
if not "%~2"=="" set "OUT=%~2"

set "PY=%USERPROFILE%\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe"
set "SCRIPT=%~dp0tools\serial_logger.py"

if not exist "%PY%" goto no_py
if not exist "%SCRIPT%" goto no_script

echo.
echo Soros logger a %PORT% porton -^> %OUT%
echo Kilepes: Ctrl + C
echo.
"%PY%" "%SCRIPT%" %PORT% "%OUT%"
exit /b %ERRORLEVEL%

:no_py
echo HIBA: nem talalom az ESP-IDF python kornyezetet:
echo   %PY%
pause
exit /b 1

:no_script
echo HIBA: nincs logger script:
echo   %SCRIPT%
pause
exit /b 1
