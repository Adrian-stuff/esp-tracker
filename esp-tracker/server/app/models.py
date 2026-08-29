from pydantic import BaseModel, Field
from typing import Literal, Optional

Source = Literal["gnss", "wifi", "ble_anchor", "cell", "manual"]

class WifiAP(BaseModel):
    bssid: str
    rssi: int
    # The only part a parent can recognise. ~20 bytes each on a 2G link, and
    # the device caps the list at 6 — negligible against a 4-6 MB/month budget.
    ssid: Optional[str] = None

class DeviceEvent(BaseModel):
    """One event from the tracker. Batched — the device sends many per
    connection so the 3-8s TLS handshake is amortised, not paid per point."""
    id: str = Field(min_length=4, max_length=64)   # device-generated; makes retries idempotent
    kind: Literal["telemetry", "sos", "geofence", "health"]
    recorded_at: int                               # unix seconds, ON THE DEVICE

    lat: Optional[float] = None
    lon: Optional[float] = None
    accuracy_m: Optional[float] = None
    source: Optional[Source] = None

    # Unresolved Wi-Fi scan. The server resolves it, because the geolocation
    # API key must never live on the device.
    wifi: Optional[list[WifiAP]] = None

    battery_pct: Optional[int] = None
    signal_csq: Optional[int] = None
    balance_pesos: Optional[int] = None
    queue_depth: Optional[int] = None
    speed_mps: Optional[float] = None

    # True when the device already texted the parent directly. The server then
    # skips its own t+0 SMS so one press never becomes two texts.
    device_sms_sent: bool = False
    is_drill: bool = False

class EventBatch(BaseModel):
    events: list[DeviceEvent] = Field(max_length=200)

class TapEvent(BaseModel):
    id: str
    card_uid: str
    recorded_at: int
    # Direction is NOT taken from the device — the server reorders the card's
    # whole day, so taps buffered offline and delivered late self-correct.
    direction: Literal["in", "out"] = "in"
    # True when the scanner already texted the parent over its SIM900. The
    # server then skips its own message: one tap must never be two texts.
    device_sms_sent: bool = False

class TapBatch(BaseModel):
    taps: list[TapEvent] = Field(max_length=200)

class LoginRequest(BaseModel):
    email: str
    password: str
    totp: Optional[str] = None

class AckRequest(BaseModel):
    note: Optional[str] = None

class CreatePlace(BaseModel):
    """A parent naming a set of networks: 'these are school'."""
    name: str = Field(min_length=1, max_length=40)
    kind: Literal["home", "school", "other"] = "other"
    bssid_hashes: list[str] = Field(min_length=1, max_length=40)
    radius_m: float = 40.0
