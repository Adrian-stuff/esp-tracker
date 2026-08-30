"""Live attendance dashboard for scanner-uno/sms_scanner.

Reads the scanner's USB serial console (its normal debug output — nothing
special enabled on the firmware side beyond the "TAP ..."/"SMS ..." lines
main.cpp already prints) and serves a live-updating web page. This exists
because sms_scanner has no server ingest path at all (see ../README.md and
../../PLAN.md §1b's GPRS section for why) — the laptop plugged into the
scanner via USB is the only place a human-readable attendance record with
names exists; the Uno's own EEPROM ring only ever holds a UID + timestamp
audit trail, deliberately (see ../sms_scanner/src/store.h) because there
is no room in 1KB of EEPROM for names too.

Two record types, both parsed from plain serial lines, both appended to a
local CSV as they arrive (so the record survives this process restarting)
and kept in memory for the web page:

  TAP uid=<hex> at=<unix epoch> name=<name-or-"-"> logged=<queue depth>
  SMS from=<number> text=<free text, rest of line>

If --server-url is set, every SMS line is ALSO relayed to the FastAPI
server's /api/relay/sms — that's how a tracker's routine "LOC ..." location
report (tracker/src/report.cpp), sent to this scanner's own number because
the tracker's GPRS is dead on the same 2G-shutdown grounds, reaches
Supabase. This dashboard doesn't parse or care about that format itself —
it forwards ALL received SMS text verbatim and lets the server (see
server/app/tracker_sms.py) decide what's a location report and what isn't.
"""
import argparse
import csv
import glob
import re
import threading
import time
from collections import deque
from datetime import datetime, timedelta, timezone
from pathlib import Path

import requests
import serial
from flask import Flask, jsonify, render_template

TAP_RE = re.compile(r"^TAP uid=(\S+) at=(\d+) name=(.*) logged=(\d+)$")
SMS_RE = re.compile(r"^SMS from=(\S+) text=(.*)$")

# The device's clock is UTC (see sms_scanner/src/modem.cpp's
# syncClockFromNetwork — NITZ time, tzQuarter-adjusted to UTC before it's
# ever stored). PH is UTC+8 — see sms_scanner/include/config.h's
# TZ_OFFSET_S, which the firmware itself never applies (RTC stores UTC by
# design, same as every other build in this project), so the conversion
# happens here instead, once, for display only.
PH_OFFSET = timedelta(hours=8)

HERE = Path(__file__).parent
ATTENDANCE_CSV = HERE / "attendance_log.csv"
SMS_CSV = HERE / "sms_log.csv"

MAX_ROWS = 500   # in-memory only — the CSVs on disk keep the full history


def ph_local(epoch: int) -> str:
    return (datetime.fromtimestamp(epoch, tz=timezone.utc) + PH_OFFSET).strftime("%Y-%m-%d %H:%M:%S")


class ScannerLink:
    """Owns the serial connection and the in-memory event buffers.

    One background thread; Flask's request threads only ever read the
    deques, guarded by `lock` — matches the one-writer/many-readers shape
    this actually has, rather than reaching for anything heavier.
    """

    def __init__(self, port: str | None, server_url: str | None = None, server_token: str | None = None):
        self.requested_port = port
        self.port_in_use: str | None = None
        self.connected = False
        self.lock = threading.Lock()
        self.attendance: deque[dict] = deque(maxlen=MAX_ROWS)
        self.messages: deque[dict] = deque(maxlen=MAX_ROWS)
        self._ser: serial.Serial | None = None   # set while connected — see run(); guarded by write_lock for writes
        self.write_lock = threading.Lock()
        self.server_url = server_url.rstrip("/") if server_url else None
        self.server_token = server_token
        self._init_csvs()

    def _relay_sms_to_server(self, sender: str, text: str, received_at: int):
        """Fire-and-forget POST to the FastAPI server's /api/relay/sms — see
        server/app/tracker_sms.py for what it does with this. Runs in its
        own short-lived thread so a slow/unreachable server can never stall
        reading the NEXT serial line; this dashboard's local CSV log (see
        _handle_line) is already the durable record regardless of whether
        this call ever succeeds.
        """
        if not self.server_url:
            return

        def _post():
            try:
                headers = {"Authorization": f"Bearer {self.server_token}"} if self.server_token else {}
                requests.post(
                    f"{self.server_url}/api/relay/sms",
                    json={"sender": sender, "text": text, "received_at": received_at},
                    headers=headers, timeout=10,
                )
            except requests.RequestException as e:
                print(f"[dashboard] relay to server failed: {e}")

        threading.Thread(target=_post, daemon=True).start()

    def send_command(self, cmd: str) -> bool:
        """Writes one line to the scanner's console (DUMP/CLEAR/SIG/CLK/
        SETTIME — see sms_scanner/src/main.cpp's serviceConsole()). False
        if not currently connected."""
        with self.write_lock:
            if not self._ser:
                return False
            try:
                self._ser.write((cmd + "\n").encode())
                return True
            except (serial.SerialException, OSError):
                return False

    def _init_csvs(self):
        if not ATTENDANCE_CSV.exists():
            with open(ATTENDANCE_CSV, "w", newline="") as f:
                csv.writer(f).writerow(["uid", "epoch", "time_ph", "name", "logged"])
        if not SMS_CSV.exists():
            with open(SMS_CSV, "w", newline="") as f:
                csv.writer(f).writerow(["received_at", "from", "text"])

    def _candidate_ports(self):
        if self.requested_port:
            return [self.requested_port]
        # Same glob order as ../flash.sh, for the same reason: prefer the
        # CH340 (wchusbserial) devices this project's boards actually use.
        for pattern in ("/dev/cu.wchusbserial*", "/dev/cu.usbserial*", "/dev/cu.usbmodem*"):
            matches = sorted(glob.glob(pattern))
            if matches:
                return matches
        return []

    def _handle_line(self, line: str):
        m = TAP_RE.match(line)
        if m:
            uid, epoch_s, name, logged = m.groups()
            epoch = int(epoch_s)
            row = {
                "uid": uid,
                "epoch": epoch,
                "time_ph": ph_local(epoch),
                "name": None if name == "-" else name,
                "logged": int(logged),
            }
            with self.lock:
                self.attendance.appendleft(row)
            with open(ATTENDANCE_CSV, "a", newline="") as f:
                csv.writer(f).writerow([uid, epoch, row["time_ph"], row["name"] or "", logged])
            return

        m = SMS_RE.match(line)
        if m:
            frm, text = m.groups()
            received_epoch = int(time.time())
            received_at = datetime.fromtimestamp(received_epoch).strftime("%Y-%m-%d %H:%M:%S")
            row = {"received_at": received_at, "from": frm, "text": text}
            with self.lock:
                self.messages.appendleft(row)
            with open(SMS_CSV, "a", newline="") as f:
                csv.writer(f).writerow([received_at, frm, text])
            self._relay_sms_to_server(frm, text, received_epoch)
            return

        # Anything else (boot line, console replies, etc.) is just the
        # scanner's normal debug output — expected, not an error, ignored.

    def run(self):
        """Reconnect loop — the port has already been seen to re-enumerate
        mid-session on this project's hardware (a USB replug renames
        /dev/cu.wchusbserial<N>), so this can't assume the port it opened
        stays valid forever."""
        while True:
            candidates = self._candidate_ports()
            if not candidates:
                self.connected = False
                time.sleep(2)
                continue
            port = candidates[0]
            try:
                with serial.Serial(port, 9600, timeout=1) as ser:
                    self.port_in_use = port
                    self._ser = ser
                    self.connected = True
                    print(f"[dashboard] connected: {port}")
                    while True:
                        raw = ser.readline()
                        if not raw:
                            continue
                        line = raw.decode(errors="replace").strip()
                        if line:
                            self._handle_line(line)
            except (serial.SerialException, OSError) as e:
                self.connected = False
                self._ser = None
                print(f"[dashboard] {port} disconnected ({e}); retrying in 2s")
                time.sleep(2)


def create_app(link: ScannerLink) -> Flask:
    app = Flask(__name__)

    @app.get("/")
    def index():
        return render_template("index.html")

    @app.get("/api/events")
    def events():
        with link.lock:
            attendance = list(link.attendance)
            messages = list(link.messages)
        return jsonify(
            connected=link.connected,
            port=link.port_in_use,
            attendance=attendance,
            messages=messages,
        )

    @app.post("/api/set_clock")
    def set_clock():
        # This laptop's own system clock, not anything from the scanner —
        # see main.cpp's SETTIME handler for why: this carrier doesn't push
        # NITZ time over AT+CCLK?, so the DS1302 has no way to self-correct
        # and needs a one-time manual set from something with an accurate
        # clock. UTC epoch — setFromEpoch() expects the same unit the
        # (unavailable, on this carrier) network path would have supplied.
        epoch = int(time.time())
        ok = link.send_command(f"SETTIME {epoch}")
        return jsonify(sent=ok, epoch=epoch)

    return app


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial-port", default=None, help="Force a specific serial port (default: auto-detect, same rule as ../flash.sh)")
    parser.add_argument("--host", default="127.0.0.1", help="Web server bind address (default: localhost-only — attendance data is real names/numbers; pass 0.0.0.0 deliberately to expose it on the LAN)")
    parser.add_argument("--http-port", type=int, default=5000, help="Web server port (default: 5000)")
    parser.add_argument("--server-url", default=None, help="FastAPI server base URL (e.g. http://localhost:8000) to relay tracker location SMS to — see server/app/tracker_sms.py. Omit to run this dashboard standalone (local CSV + web view only, no server relay).")
    parser.add_argument("--server-token", default=None, help="This scanner's own device bearer token (matches its devices.token_hash row) — required if --server-url is set.")
    args = parser.parse_args()

    link = ScannerLink(args.serial_port, args.server_url, args.server_token)
    threading.Thread(target=link.run, daemon=True).start()

    app = create_app(link)
    print(f"[dashboard] serving on http://{args.host}:{args.http_port}")
    app.run(host=args.host, port=args.http_port)


if __name__ == "__main__":
    main()
