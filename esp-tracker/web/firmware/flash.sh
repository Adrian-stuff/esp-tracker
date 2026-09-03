#!/data/data/com.termux/files/usr/bin/bash
# ============================================================
# Tracker Firmware Flash — Android (Termux)
# One-shot script: installs Python + esptool, downloads
# firmware, detects ESP32, and flashes.
#
# USAGE:
#   1. Install Termux from F-Droid (NOT Play Store — outdated)
#   2. Install Termux:USB from F-Droid
#   3. Open Termux, paste this entire script, press Enter
#   4. Plug ESP32 tracker into phone via USB OTG
#   5. Follow prompts
# ============================================================

set -e

FIRMWARE_URL="https://web-ten-beta-17.vercel.app/firmware/firmware.bin"
FIRMWARE_BIN="$HOME/firmware.bin"

echo ""
echo "========================================"
echo " Tracker Firmware Flash (Android)"
echo "========================================"
echo ""

# ------------------------------------------
# Step 1: Install Python
# ------------------------------------------
echo "[1/5] Checking Python..."
if ! command -v python3 &>/dev/null; then
    echo "  Installing Python..."
    pkg update -y >/dev/null 2>&1
    pkg install -y python >/dev/null 2>&1
fi
echo "  Python: $(python3 --version 2>&1)"

# ------------------------------------------
# Step 2: Install esptool
# ------------------------------------------
echo ""
echo "[2/5] Checking esptool..."
if ! python3 -m esptool version &>/dev/null; then
    echo "  Installing esptool..."
    pip install esptool >/dev/null 2>&1
fi
echo "  esptool: $(python3 -m esptool version 2>&1 | head -1)"

# ------------------------------------------
# Step 3: Download firmware
# ------------------------------------------
echo ""
echo "[3/5] Downloading firmware..."
if [ -f "$FIRMWARE_BIN" ]; then
    echo "  firmware.bin already exists at $FIRMWARE_BIN"
    read -p "  Re-download? [y/N] " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        rm -f "$FIRMWARE_BIN"
    fi
fi

if [ ! -f "$FIRMWARE_BIN" ]; then
    # Try curl, fall back to wget
    if command -v curl &>/dev/null; then
        curl -L -o "$FIRMWARE_BIN" "$FIRMWARE_URL"
    elif command -v wget &>/dev/null; then
        wget -O "$FIRMWARE_BIN" "$FIRMWARE_URL"
    else
        pkg install -y curl >/dev/null 2>&1
        curl -L -o "$FIRMWARE_BIN" "$FIRMWARE_URL"
    fi

    if [ ! -f "$FIRMWARE_BIN" ]; then
        echo "  FAILED to download firmware. Check your internet connection."
        echo "  Manual download: $FIRMWARE_URL"
        exit 1
    fi
fi

FWSIZE=$(wc -c < "$FIRMWARE_BIN" | tr -d ' ')
echo "  Downloaded: firmware.bin ($FWSIZE bytes)"

# ------------------------------------------
# Step 4: Request USB permission
# ------------------------------------------
echo ""
echo "[4/5] Looking for ESP32 on USB..."

# Termux:USB exposes devices at /dev/bus/usb/
# We need to find the ESP32's USB serial device
DEV=""

# Method 1: Check /dev/ttyUSB* or /dev/ttyACM* (most common)
for port in /dev/ttyUSB* /dev/ttyACM*; do
    if [ -e "$port" ]; then
        # Quick probe — esptool will fail fast if it's not an ESP32
        if python3 -m esptool --port "$port" chip_id &>/dev/null 2>&1; then
            DEV="$port"
            break
        fi
    fi
done

# Method 2: Check /dev/bus/usb/ directly
if [ -z "$DEV" ]; then
    for usb in /dev/bus/usb/*/*; do
        if [ -e "$usb" ]; then
            if python3 -m esptool --port "$usb" chip_id &>/dev/null 2>&1; then
                DEV="$usb"
                break
            fi
        fi
    done
fi

if [ -z "$DEV" ]; then
    echo ""
    echo "  ============================================"
    echo "   No ESP32 found on USB."
    echo "  ============================================"
    echo ""
    echo "  Make sure:"
    echo "    1. Tracker is plugged in via USB OTG cable"
    echo "    2. Termux:USB is installed and USB permission granted"
    echo "    3. When prompted by Android, tap 'OK' to allow Termux USB access"
    echo "    4. Try a different USB OTG cable (some are charge-only)"
    echo "    5. Hold the BOOT button on the tracker while plugging in"
    echo ""
    echo "  Re-run this script after fixing."
    exit 1
fi

echo "  Found ESP32 on $DEV"

# ------------------------------------------
# Step 5: Flash
# ------------------------------------------
echo ""
echo "[5/5] Flashing firmware to $DEV..."
echo "  (Hold BOOT button on tracker if flashing doesn't start)"
echo ""

python3 -m esptool --chip esp32 --port "$DEV" --baud 460800 write_flash -z 0x0 "$FIRMWARE_BIN"

echo ""
echo "========================================"
echo " Flash complete! Tracker is rebooting."
echo "========================================"
echo ""
echo " Next steps:"
echo "   1. Connect to tracker WiFi: 'Tracker-Setup'"
echo "   2. Open: http://192.168.4.1"
echo "   3. Configure SOS number, scanner, WiFi"
echo "   4. Hold SOS button 2s to test"
echo ""
