"""Parses and verifies SMS formats from the tracker firmware.

Two formats are supported:

1. LOC — routine position report (see parse())
2. WIFISCAN — WiFi BSSID scan for server-side place matching (see parse_wifi_scan())

Both use the same truncated-SHA256-over-a-shared-secret verification.
The tracker never talks to this server directly: it texts a scanner's
SIM800L, which relays whatever it receives here — see scanner-uno/
sms_scanner/src/modem.h's pollSms() and main.py's /api/relay/sms.
"""
import hashlib
import re

from .config import SMS_CMD_SECRET

SOURCE_CODES = {"g": "gnss", "w": "wifi", "b": "ble_anchor", "c": "cell"}

_LOC_RE = re.compile(
    r"^LOC ([gwbc]),(-?\d+\.\d+),(-?\d+\.\d+),(\d+(?:\.\d+)?),(\d+),([0-9a-f]{8})$"
)


def _code(payload: str) -> str:
    return hashlib.sha256((SMS_CMD_SECRET + payload).encode()).hexdigest()[:8]


def parse(text: str):
    """Returns (source, lat, lon, accuracy_m, recorded_at) for a validly
    formatted AND correctly-coded LOC report, else None.

    Verification happens here, not left to the caller — one place that can
    get it wrong, not two. A format mismatch and a bad code both come back
    as None rather than distinct errors: this channel's SMS could just as
    easily be a wrong number or a stray text as an actual spoofing attempt,
    and this project's security posture elsewhere (see roster_hash's own
    comment) doesn't call for a full alerting pipeline over that
    distinction at this scale.
    """
    m = _LOC_RE.match(text)
    if not m:
        return None
    src, lat_s, lon_s, acc_s, epoch_s, code = m.groups()
    payload = f"{src},{lat_s},{lon_s},{acc_s},{epoch_s}"
    if _code(payload) != code:
        return None
    return SOURCE_CODES[src], float(lat_s), float(lon_s), float(acc_s), int(epoch_s)


def parse_wifi_scan(text: str):
    """Returns (recorded_at, aps_list) for a valid WIFISCAN report, else None.

    aps_list is a list of dicts: [{"bssid": "AA:BB:CC:DD:EE:FF", "rssi": -45, "ssid": "Network"}, ...]

    The code is verified the same way as LOC: sha256(SMS_CMD_SECRET + payload)[:8].
    The payload is "<epoch>,<bssid>:<rssi>:<ssid>,<bssid>:<rssi>:<ssid>,...".
    """
    if not text.startswith("WIFISCAN "):
        return None
    body = text[len("WIFISCAN "):]

    # Split off the trailing code (last 9 chars: comma + 8 hex)
    if len(body) < 10:
        return None
    payload_part, _, code = body.rpartition(",")
    if len(code) != 8 or not all(c in '0123456789abcdef' for c in code):
        return None

    if _code(payload_part) != code:
        return None

    # Parse the payload: <epoch>,<bssid>:<rssi>:<ssid>,...
    parts = payload_part.split(",", 1)
    if len(parts) < 2:
        return None
    try:
        recorded_at = int(parts[0])
    except ValueError:
        return None

    aps = []
    for ap_str in parts[1].split(","):
        # Format: AA:BB:CC:DD:EE:FF:<rssi>:<ssid>
        segments = ap_str.split(":", 7)
        if len(segments) < 7:
            continue
        bssid = ":".join(segments[:6])
        try:
            rssi = int(segments[6])
        except ValueError:
            continue
        ssid = segments[7] if len(segments) > 7 else ""
        aps.append({"bssid": bssid, "rssi": rssi, "ssid": ssid})

    if not aps:
        return None
    return recorded_at, aps
