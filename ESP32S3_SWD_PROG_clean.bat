@echo off
rem ==========================================================================
rem  ESP32S3_SWD_PROG - TISZTITAS (idf.py fullclean + sdkconfig torles).
rem  Akkor kell, ha az sdkconfig.defaults vagy a partitions.csv valtozott
rem  (a sdkconfig ujragenralodik a defaults-bol a kovetkezo build.bat-nal).
rem ==========================================================================
setlocal

set "IDF=%USERPROFILE%\esp\v5.5.1\esp-idf"
if not exist "%IDF%\export.bat" goto no_idf

call "%IDF%\export.bat"
if errorlevel 1 goto env_err

rem cd a projekt-mappaba (a path szokozei miatt nem -C argumentumot hasznalunk).
cd /d "%~dp0."
idf.py fullclean
if exist sdkconfig del /q sdkconfig
echo.
echo KESZ - build mappa torolve + sdkconfig torolve. Kovetkezo: build.bat
exit /b 0

:env_err
echo HIBA: az ESP-IDF kornyezet betoltese sikertelen.
pause
exit /b 1

:no_idf
echo HIBA: nem talalom az ESP-IDF export.bat-ot:
echo   %IDF%\export.bat
pause
exit /b 1
