@echo off
setlocal enabledelayedexpansion
pushd "%~dp0"
cls
color f0
echo.
echo ===============================================
echo   WROOM A2DP TX - Bluetooth Firmware Flasher   
echo             WROOM A2DP TX                      
echo ===============================================
echo.
echo   Author: A. Jaroszuk
echo.
echo.

:selectCOM
echo Available COM-ports:
echo.
set "_key=HKEY_LOCAL_MACHINE\HARDWARE\DEVICEMAP\SERIALCOMM"
set "_found=0"

for /f "tokens=3 delims= " %%c in ('reg query "!_key!" 2^>NUL ^| find /v "HKEY"') do (
    set "_com_N=%%c"
    set "_port_num=!_com_N:~3!"
    echo   !_port_num! = !_com_N!
    set "_found=1"
)

if "!_found!"=="0" (
    echo   No COM ports found!
    echo.
)

echo.
set /p "COM=Enter WROOM COM-port number (example: 5, or X to exit): "

if /i "!COM!"=="X" goto exit_script
if /i "!COM!"=="EXIT" goto exit_script

if "!COM!"=="" (
    echo.
    echo Error: Please enter a COM-port number!
    echo.
    goto selectCOM
)

set "validCOM="
for /f "tokens=3 delims= " %%c in ('reg query "!_key!" 2^>NUL ^| find /v "HKEY"') do (
    set "_com_N=%%c"
    set "_port_num=!_com_N:~3!"
    if /i "!_port_num!"=="!COM!" set "validCOM=1"
)

if not defined validCOM (
    echo.
    echo Error: COM!COM! is not available or invalid!
    echo Please enter a valid COM-port number from the list.
    echo.
    goto selectCOM
)

echo.
echo ===============================================
echo Uploading firmware to COM!COM!...
echo ===============================================
echo.

REM Check if esptool is available
if exist "%~dp0esptool.exe" (
    set "_esptool=%~dp0esptool.exe"
) else (
    where esptool.exe >NUL 2>&1
    if %ERRORLEVEL% equ 0 (
        set "_esptool=esptool.exe"
    ) else (
        where esptool.py >NUL 2>&1
        if %ERRORLEVEL% equ 0 (
            set "_esptool=python -m esptool"
        ) else (
            echo.
            echo Error: esptool not found!
            echo Expected local file: esptool.exe in this folder.
            echo Or install globally: pip install esptool
            echo.
            pause
            popd
            exit /b 1
        )
    )
)

REM Flash firmware
call %_esptool% --chip esp32 --port COM!COM! --baud 921600 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq keep --flash_size detect 0x1000 bootloader.bin 0x8000 partitions.bin 0x10000 app.bin
set "FLASH_RC=%ERRORLEVEL%"

if not "%FLASH_RC%"=="0" (
    echo.
    echo ===============================================
    echo Error uploading firmware (exit code %FLASH_RC%)!
    echo ===============================================
    echo.
    echo Possible causes:
    echo - Wrong COM-port selected
    echo - WROOM not in USB mode (check USB cable)
    echo - Driver not installed
    echo - esptool not installed (pip install esptool)
    echo.
    echo If problem persists:
    echo 1. Hold BOOT button on WROOM
    echo 2. Click RST button while holding BOOT
    echo 3. Release BOOT button
    echo 4. Try flashing again
    echo.
    echo Press any key to return to COM-port selection.
    pause >NUL
    goto selectCOM
)

echo.
echo ===============================================
echo Firmware upload completed successfully!
echo ===============================================
echo.
echo Next steps:
echo 1. WROOM will reboot automatically
echo 2. Open serial monitor at 115200 baud
echo 3. You should see: "READY WROOM"
echo 4. Connect to BT ON command via UART
echo.
echo Press any key to close this program.
pause >NUL

:exit_script
popd
endlocal
exit /b 0
