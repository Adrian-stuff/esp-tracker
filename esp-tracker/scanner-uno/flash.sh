#!/usr/bin/env bash
set -euo pipefail

# Flashes one of this project's Uno firmwares to whatever board is plugged
# in. Exists because testing this project means constantly swapping the
# SAME physical Uno between roles — flash the writer, enroll a card, flash
# back to the scanner to test reading it — and each role is its own
# PlatformIO project (own platformio.ini/src/), so "just re-flash" means
# "cd to the right directory first, and free the port if a monitor script
# is still holding it open." This wraps that up into one command.
#
# Usage:
#   ./flash.sh main     - the real gate-scanner firmware (src/) — GPRS+SMS, gateway mode
#   ./flash.sh sms      - sms_scanner/ — SMS-only firmware, no GPRS/HTTP at all
#   ./flash.sh writer   - card_writer/ (enrollment station, no GPS/GPRS)
#   ./flash.sh demo     - card_demo/ (modem-free RFID+LCD bench demo)
#
# Env overrides:
#   SCANNER_PORT=/dev/cu.xxxx   force a specific serial port
#   PIO_BIN=/path/to/pio        force a specific `pio` executable

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

case "${1:-}" in
    main)   PROJECT="$SCRIPT_DIR" ;;
    sms)    PROJECT="$SCRIPT_DIR/sms_scanner" ;;
    writer) PROJECT="$SCRIPT_DIR/card_writer" ;;
    demo)   PROJECT="$SCRIPT_DIR/card_demo" ;;
    *)
        echo "Usage: $0 {main|sms|writer|demo}"
        echo "  main   - gate scanner firmware (reads cards, drives SIM800L/relay over GPRS)"
        echo "  sms    - SMS-only gate scanner (reads cards, texts straight off the card, no GPRS/HTTP)"
        echo "  writer - card enrollment station (talks to card_writer_gui.py)"
        echo "  demo   - modem-free RFID+LCD bench demo, for quick card checks"
        exit 1
        ;;
esac

# Find `pio`: PATH first, then the location this project's been using in
# environments where it was only pip-installed under the user's own
# ~/Library (no system-wide install, not on PATH).
PIO="${PIO_BIN:-}"
if [ -z "$PIO" ]; then
    if command -v pio >/dev/null 2>&1; then
        PIO="pio"
    elif [ -x "$HOME/Library/Python/3.14/bin/pio" ]; then
        PIO="$HOME/Library/Python/3.14/bin/pio"
    else
        echo "error: 'pio' not found. Install with: pip install platformio" >&2
        echo "       or set PIO_BIN=/path/to/pio" >&2
        exit 1
    fi
fi

# Find the board: SCANNER_PORT if set, else the first likely-looking Uno
# device. Avoids relying on any external command (head/awk/etc.) for the
# pick, just in case the invoking shell's PATH is unusual.
PORT="${SCANNER_PORT:-}"
if [ -z "$PORT" ]; then
    for candidate in /dev/cu.wchusbserial* /dev/cu.usbserial* /dev/cu.usbmodem*; do
        if [ -e "$candidate" ]; then PORT="$candidate"; break; fi
    done
fi
if [ -z "$PORT" ] || [ ! -e "$PORT" ]; then
    echo "error: no Uno found. Plug it in, or set SCANNER_PORT=/dev/cu.xxxx" >&2
    exit 1
fi

# Free the port if something else (a serial monitor, a leftover python
# logging script, etc.) still has it open — avrdude needs exclusive access.
if command -v lsof >/dev/null 2>&1; then
    HOLDER_PID="$(lsof -t "$PORT" 2>/dev/null || true)"
    if [ -n "$HOLDER_PID" ]; then
        echo "Freeing $PORT (was held by pid $HOLDER_PID)..."
        kill $HOLDER_PID 2>/dev/null || true
        sleep 1
    fi
fi

echo "Flashing '$1' ($PROJECT) to $PORT..."
cd "$PROJECT"
"$PIO" run -e uno -t upload --upload-port "$PORT"
