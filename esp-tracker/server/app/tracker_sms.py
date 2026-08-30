"""Parses and verifies the "LOC ..." SMS format tracker firmware sends when
GPRS/HTTP isn't available on this hardware (SIM800L, 2G-only, confirmed
dead under the Philippines' NTC-mandated 2G/3G shutdown — see PLAN.md §1b
and tracker/src/modem.cpp's file header). The tracker never talks to this
server directly: it texts a scanner's SIM800L, which relays whatever it
receives here — see scanner-uno/sms_scanner/src/modem.h's pollSms() and
scanner-uno/dashboard/app.py, the only caller of the endpoint that uses
this module (main.py's /api/relay/sms).

Wire format (must match tracker/src/report.cpp byte for byte):

    LOC <src>,<lat>,<lon>,<acc_m>,<epoch>,<code>

src is a single letter for locator.h's Fix::source, compressed to fit an
SMS: g=gnss, w=wifi, b=ble_anchor, c=cell.

code is the first 8 hex chars of sha256(SMS_CMD_SECRET + "<src>,<lat>,<lon>,<acc_m>,<epoch>")
— that exact substring, no "LOC " prefix, no trailing comma. Verified
against the RAW TEXT the tracker sent (via the regex capture groups below),
not a re-serialized float, so a platform formatting difference between the
ESP32's snprintf and Python's float repr can never cause a false mismatch.
Same truncated-SHA256-over-a-shared-secret shape as attendance.py's
roster_hash() — a deterrent against someone texting a scanner's number
pretending to be a tracker, not real end-to-end cryptography. Proportionate
to what an SMS channel and an 8-bit-adjacent MCU can actually offer, same
honesty as this project's TLS notes elsewhere — see SMS_CMD_SECRET's own
comment in tracker/include/config.h.
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
