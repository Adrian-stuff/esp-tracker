@echo off
setlocal enabledelayedexpansion

:: ============================================================
:: Tracker Firmware Flash Tool
:: Standalone — installs Python + PlatformIO + USB drivers,
:: then flashes firmware.bin via USB.
::
:: USAGE:
::   1. Place this file + firmware.bin in the same folder
::   2. Double-click this file
::   3. Plug tracker into USB, hold BOOT if needed
:: ============================================================

title Tracker Firmware Flash

set "FIRMWARE_BIN=%~dp0firmware.bin"
set "PYTHON_URL=https://www.python.org/ftp/python/3.12.4/python-3.12.4-amd64.exe"
set "PYTHON_INSTALLER=%TEMP%\python-installer.exe"

echo.
echo ========================================
echo  Tracker Firmware Flash Tool
echo ========================================
echo.

:: ------------------------------------------
:: Step 1: Check firmware.bin exists
:: ------------------------------------------
echo [1/7] Checking firmware...

if not exist "%FIRMWARE_BIN%" (
    echo  ERROR: firmware.bin not found next to this script.
    echo.
    echo  Place firmware.bin in the same folder as this .bat file:
    echo    %~dp0
    echo.
    echo  Build it yourself with:
    echo    cd esp-tracker ^&^& pio run -e tracker
    echo  Then copy: tracker\.pio\build\tracker\firmware.bin
    echo.
    pause
    exit /b 1
)

for %%A in ("%FIRMWARE_BIN%") do set FWSIZE=%%~zA
echo  Found firmware.bin: !FWSIZE! bytes

:: ------------------------------------------
:: Step 2: Check / Install Python
:: ------------------------------------------
echo.
echo [2/7] Checking Python...

python --version >nul 2>&1
if %errorlevel% neq 0 (
    echo  Python NOT found. Downloading Python 3.12.4...
    echo  An installer window will open — follow the prompts.
    echo.
    powershell -Command "Invoke-WebRequest -Uri '%PYTHON_URL%' -OutFile '%PYTHON_INSTALLER%'"
    if exist "%PYTHON_INSTALLER%" (
        echo  Running installer (silent, adds to PATH)...
        "%PYTHON_INSTALLER%" /passive InstallAllUsers=0 PrependPath=1 Include_test=0
        echo  Installer finished. Refreshing PATH...
        set "PATH=%LOCALAPPDATA%\Programs\Python\Python312;%LOCALAPPDATA%\Programs\Python\Python312\Scripts;%PATH%"
        del "%PYTHON_INSTALLER%" 2>nul
    ) else (
        echo  FAILED to download Python. Install manually from:
        echo  https://www.python.org/downloads/
        pause
        exit /b 1
    )
) else (
    for /f "tokens=2 delims= " %%v in ('python --version 2^>^&1') do set PYVER=%%v
    echo  Found Python !PYVER!
)

:: Verify pip
python -m pip --version >nul 2>&1
if %errorlevel% neq 0 (
    echo  pip not available. Installing...
    python -m ensurepip --upgrade
)

:: ------------------------------------------
:: Step 3: Install PlatformIO
:: ------------------------------------------
echo.
echo [3/7] Checking PlatformIO...

pio --version >nul 2>&1
if %errorlevel% neq 0 (
    echo  PlatformIO not found. Installing via pip...
    python -m pip install --user platformio
    set "PATH=%USERPROFILE%\.platformio\penv\Scripts;%PATH%"
    pio --version >nul 2>&1
    if !errorlevel! neq 0 (
        echo  FAILED to install PlatformIO. Try manually:
        echo    python -m pip install platformio
        pause
        exit /b 1
    )
) else (
    for /f "tokens=2 delims= " %%v in ('pio --version 2^>^&1') do set PIOVER=%%v
    echo  Found PlatformIO !PIOVER!
)

:: ------------------------------------------
:: Step 4: Install esptool
:: ------------------------------------------
echo.
echo [4/7] Checking esptool...

python -m esptool.py version >nul 2>&1
if %errorlevel% neq 0 (
    echo  esptool not found. Installing...
    python -m pip install --user esptool
)

:: ------------------------------------------
:: Step 5: Install USB drivers
:: ------------------------------------------
echo.
echo [5/7] Checking USB drivers...

:: Check if any ESP32 is already visible
set "DRIVER_OK="
for /l %%N in (1,1,15) do (
    python -m esptool.py --port COM%%N chip_id >nul 2>&1
    if !errorlevel! equ 0 (
        set "DRIVER_OK=1"
    )
)

if not defined DRIVER_OK (
    echo.
    echo  No ESP32 detected yet. If this is the first time:
    echo.
    echo  Install the USB driver for your tracker's USB bridge chip:
    echo    CH340 (most common): https://www.wch-ic.com/downloads/CH341SER_EXE.html
    echo    CP2102:              https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers
    echo.
    echo  After installing the driver, plug in the tracker and re-run this script.
    echo.
) else (
    echo  USB drivers OK.
)

:: ------------------------------------------
:: Step 6: Find ESP32 serial port
:: ------------------------------------------
echo.
echo [6/7] Scanning for ESP32 on USB...

set "COMPORT="

:: Try pio device list first
for /f "tokens=1 delims=-" %%P in ('pio device list 2^>nul ^| findstr /i "COM"') do (
    if not defined COMPORT (
        set "RAW=%%P"
        for /f "tokens=* delims= " %%T in ("!RAW!") do set "COMPORT=%%T"
    )
)

:: Fallback: probe each COM port
if not defined COMPORT (
    echo  Probing COM ports...
    for /l %%N in (1,1,15) do (
        if not defined COMPORT (
            python -m esptool.py --port COM%%N chip_id >nul 2>&1
            if !errorlevel! equ 0 (
                set "COMPORT=COM%%N"
            )
        )
    )
)

if not defined COMPORT (
    echo.
    echo  ============================================
    echo   No ESP32 found on any COM port.
    echo  ============================================
    echo.
    echo  Make sure:
    echo    1. Tracker is plugged into USB (data cable, not charge-only)
    echo    2. USB drivers are installed (see step 5)
    echo    3. No other program is using the port
    echo    4. Try holding the BOOT button on the tracker
    echo.
    echo  Re-run this script after fixing.
    pause
    exit /b 1
)

echo  Found tracker on %COMPORT%

:: ------------------------------------------
:: Step 7: Flash
:: ------------------------------------------
echo.
echo [7/7] Flashing firmware to %COMPORT%...
echo  (Hold BOOT button on tracker if flashing doesn't start)
echo.

python -m esptool.py --chip esp32 --port "%COMPORT%" --baud 921600 write_flash -z 0x0 "%FIRMWARE_BIN%"
if %errorlevel% neq 0 (
    echo.
    echo  FLASH FAILED. Try:
    echo    1. Hold BOOT button on tracker while flashing starts
    echo    2. Use a data cable, not charge-only
    echo    3. Try a different USB port
    echo    4. Reinstall USB drivers
    pause
    exit /b 1
)

echo.
echo ========================================
echo  Flash complete! Tracker is rebooting.
echo ========================================
echo.
echo  Next steps:
echo    1. Connect to tracker WiFi: "Tracker-Setup"
echo    2. Open: http://192.168.4.1
echo    3. Configure SOS number, scanner, WiFi
echo    4. Hold SOS button 2s to test
echo.
pause
