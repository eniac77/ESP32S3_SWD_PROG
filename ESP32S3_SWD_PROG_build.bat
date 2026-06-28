@echo off
rem ==========================================================================
rem  ESP32S3_SWD_PROG - FORDITAS (idf.py build).
rem  Behivja az ESP-IDF export.bat-ot (env: cmake, ninja, toolchain, python),
rem  majd a projektre fordit. Elso futaskor lassu (env setup).
rem  Utana flashelni: flash.bat (app) vagy fullflash.bat (minden).
rem ==========================================================================
setlocal

set "IDF=%USERPROFILE%\esp\v5.5.1\esp-idf"
if not exist "%IDF%\export.bat" goto no_idf

call "%IDF%\export.bat"
if errorlevel 1 goto env_err

rem A projekt-mappaba lepunk (a path "--- CLAUDE ---" szokozei miatt a
rem -C "<path>" argumentumot az idf.py wrapper feldarabolja, ezert cd + -C nelkul).
cd /d "%~dp0."
idf.py build
set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" goto build_err
echo KESZ - a forditas sikeres. Flash: flash.bat / fullflash.bat
exit /b 0

:build_err
echo HIBA - a build sikertelen (kod: %RC%).
pause
exit /b %RC%

:env_err
echo HIBA: az ESP-IDF kornyezet betoltese sikertelen.
pause
exit /b 1

:no_idf
echo HIBA: nem talalom az ESP-IDF export.bat-ot:
echo   %IDF%\export.bat
pause
exit /b 1
