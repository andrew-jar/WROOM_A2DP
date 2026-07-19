@echo off
setlocal enabledelayedexpansion
pushd "%~dp0"
cls
color f0

echo ===============================================
echo   WROOM A2DP TX - Bluetooth Firmware Flasher   
echo             WROOM A2DP TX                      
echo ===============================================
echo.
echo   Author: A. Jaroszuk
echo.
echo.

if exist "%~dp0esptool.exe" (
    set "ESPTOOL=%~dp0esptool.exe"
) else (
    where esptool.exe >NUL 2>&1
    if %ERRORLEVEL% neq 0 (
        echo esptool.exe not found.
        echo Put esptool.exe in this folder or add it to PATH.
        echo.
        echo Press any key to close.
        pause >NUL
        popd
        exit /b 1
    )
    set "ESPTOOL=esptool.exe"
)

:selectCOM
echo Available COM ports:
set "_key=HKEY_LOCAL_MACHINE\HARDWARE\DEVICEMAP\SERIALCOMM"
for /f "tokens=3 delims= " %%c in ('reg query "!_key!" 2^>NUL ^| find /v "HKEY"') do (
    set "_com_N=%%c"
    echo !_com_N:~3! = !_com_N!
)

echo.
set /p "COM=Enter COM number (e.g. 5, or X to exit): "
if /i "!COM!"=="X" goto exit_script
if /i "!COM!"=="EXIT" goto exit_script

set "validCOM="
for /f "tokens=3 delims= " %%c in ('reg query "!_key!" 2^>NUL ^| find /v "HKEY"') do (
    set "_com_N=%%c"
    if /i "!_com_N:~3!"=="!COM!" set "validCOM=1"
)

if not defined validCOM (
    echo.
    echo Invalid COM port. Try again.
    echo.
    goto selectCOM
)

echo.
echo Uploading firmware to COM!COM!...
call "%ESPTOOL%" --chip esp32 --port COM!COM! --baud 921600 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq keep --flash_size detect 0x1000 bootloader.bin 0x8000 partitions.bin 0x10000 app.bin
set "FLASH_RC=%ERRORLEVEL%"

if not "%FLASH_RC%"=="0" (
    echo.
    echo ERROR: Flashing failed.
    echo Exit code: %FLASH_RC%
    echo Check COM port, USB driver, and device BOOT mode.
    echo.
    echo Press any key to try again.
    pause >NUL
    goto selectCOM
)

echo.
echo SUCCESS: Flashing completed successfully.
echo.
echo Press any key to close.
pause >NUL

:exit_script
popd
endlocal
exit /b 0
