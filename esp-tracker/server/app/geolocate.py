"""Wi-Fi BSSID -> coordinates.

This runs on the SERVER, never on the device: the API key must not ship inside
something a child carries, and routing it here means the device only ever sends
a BSSID list over a channel we control.

Known-place matching comes first and usually wins. When it does, no API call
happens at all — which is simultaneously the cheapest, the fastest, and the
most private outcome, because the provider never learns the child's routine.
"""
import hashlib, json
from . import db
from .config import GEOLOCATION_API_KEY

def bssid_hash(bssid: str) -> str:
    """Stable, non-reversible id for an access point. Places match on these, so
    the raw BSSIDs never need to be kept."""
    return hashlib.sha256(bssid.lower().encode()).hexdigest()[:16]

_h = bssid_hash

RETAIN_SCANS_S = 7 * 86400

def record_scan(device_id: str, aps: list[dict], recorded_at: int,
                place_id: int | None) -> None:
    """Keep a short window of scans so the dashboard can offer them for naming.
    Seven days is enough to recognise a routine and short enough to limit what a
    breach exposes."""
    payload = json.dumps([{"h": _h(a["bssid"]), "ssid": (a.get("ssid") or "")[:32],
                           "rssi": a.get("rssi")} for a in aps])
    db.execute("INSERT INTO wifi_scans (device_id,recorded_at,aps,place_id) VALUES (?,?,?,?)",
               (device_id, recorded_at, payload, place_id))
    db.execute("DELETE FROM wifi_scans WHERE device_id=? AND recorded_at < ?",
               (device_id, recorded_at - RETAIN_SCANS_S))

def match_known_place(device_id: str, aps: list[dict]) -> tuple[dict | None, int | None]:
    """A place matches when enough of its registered BSSIDs are visible."""
    seen = {_h(a["bssid"]) for a in aps}
    for p in db.query("SELECT * FROM places WHERE device_id=? AND bssid_set IS NOT NULL",
                      (device_id,)):
        try:
            registered = set(json.loads(p["bssid_set"]))
        except Exception:
            continue
        if registered and len(seen & registered) >= max(2, len(registered) // 3):
            return ({"lat": p["lat"], "lon": p["lon"],
                     "accuracy_m": p["radius_m"] or 30.0,
                     "source": "ble_anchor" if p["ble_anchor_id"] else "wifi"}, p["id"])
    return None, None

async def resolve(device_id: str, aps: list[dict]) -> tuple[dict | None, int | None]:
    place, place_id = match_known_place(device_id, aps)
    if place:
        return place, place_id
    if not GEOLOCATION_API_KEY or not aps:
        return None, None
    # TODO: POST to the provider (Unwired Labs / Combain / Google). Cache the
    # result keyed on the BSSID set — a 2-minute cadence while moving gets
    # expensive fast otherwise.
    return None, None
