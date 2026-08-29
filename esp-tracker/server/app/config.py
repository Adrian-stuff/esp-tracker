import os
from pathlib import Path

def _env(key: str, default: str = "") -> str:
    return os.environ.get(key, default)

DB_PATH   = Path(_env("DB_PATH", "./tracker.db"))
SCHEMA    = Path(__file__).resolve().parent.parent / "schema.sql"
STATIC    = Path(__file__).resolve().parent.parent / "static"

SMS_PROVIDER          = _env("SMS_PROVIDER", "console")
SEMAPHORE_API_KEY     = _env("SEMAPHORE_API_KEY")
SEMAPHORE_SENDER_NAME = _env("SEMAPHORE_SENDER_NAME")
TWILIO_ACCOUNT_SID    = _env("TWILIO_ACCOUNT_SID")
TWILIO_AUTH_TOKEN     = _env("TWILIO_AUTH_TOKEN")
TWILIO_FROM           = _env("TWILIO_FROM")

SMS_CMD_SECRET        = _env("SMS_CMD_SECRET", "change-me")
GEOLOCATION_API_KEY   = _env("GEOLOCATION_API_KEY")

SESSION_TTL_HOURS     = int(_env("SESSION_TTL_HOURS", "12"))

# Session cookies are Secure by default, so they are refused over plain HTTP.
# That is correct in production and only correct there — set COOKIE_SECURE=0 for
# local development, never on anything reachable from the internet.
COOKIE_SECURE         = _env("COOKIE_SECURE", "1") != "0"

# The escalation ladder. These are the safety contract, not tuning knobs —
# see PLAN.md 2.3. Each step fires only if the SOS is still unacknowledged.
ESCALATION = [
    (0,   "push"),    # + websocket to any open dashboard
    (0,   "sms"),     # skipped if the DEVICE already texted the parent directly
    (60,  "sms2"),    # secondary contact
    (180, "voice"),   # automated call, primary
    (300, "voice2"),  # automated call, secondary
]

# Device-relayed SMS. Both modems are paid for already; the provider becomes the
# guarantee rather than the default. SOS never uses this path — see relay.py.
RELAY_ENABLED      = _env("RELAY_ENABLED", "1") != "0"
RELAY_LEASE_S      = int(_env("RELAY_LEASE_S", "60"))      # claim lifetime
RELAY_FALLBACK_S   = int(_env("RELAY_FALLBACK_S", "90"))   # then the server pays
RELAY_MAX_ATTEMPTS = int(_env("RELAY_MAX_ATTEMPTS", "2"))
RELAY_SWEEP_S      = int(_env("RELAY_SWEEP_S", "20"))

# A position older than this is rendered as stale, never as "here".
STALE_AFTER_S = 15 * 60
