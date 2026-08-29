#!/usr/bin/env python3
"""One command to get a working local stack with test data.

    python3 seed.py

Prints the credentials and device tokens to paste into the firmware configs.
Safe to re-run: it recreates the database from scratch.
"""
import os, sys, time, secrets, pathlib

os.environ.setdefault("DB_PATH", "./tracker.db")
os.environ.setdefault("SMS_PROVIDER", "console")
os.environ.setdefault("COOKIE_SECURE", "0")

for suffix in ("", "-wal", "-shm"):
    p = pathlib.Path(os.environ["DB_PATH"] + suffix)
    if p.exists():
        p.unlink()

from app import db, auth          # noqa: E402
db.init()

NOW = int(time.time())
PASSWORD = "tracker-dev"

uid = db.execute(
    """INSERT INTO users (email,password_hash,role,phone,escalation_order,created_at)
       VALUES (?,?,?,?,?,?)""",
    ("parent@example.com", auth.hash_password(PASSWORD), "parent", "+639171234567", 1, NOW))
uid2 = db.execute(
    """INSERT INTO users (email,password_hash,role,phone,escalation_order,created_at)
       VALUES (?,?,?,?,?,?)""",
    ("lola@example.com", auth.hash_password(PASSWORD), "parent", "+639177654321", 2, NOW))

tracker_token = secrets.token_urlsafe(24)
scanner_token = secrets.token_urlsafe(24)

db.execute("""INSERT INTO devices (id,kind,name,child_name,token_hash,msisdn,created_at,
                                   battery_pct,signal_csq,balance_pesos)
              VALUES (?,?,?,?,?,?,?,?,?,?)""",
           ("tracker-01", "tracker", "Tracker 01", "Ana",
            auth.device_token_hash(tracker_token), "+639181112222", NOW, 74, 18, 45))
db.execute("""INSERT INTO devices (id,kind,name,token_hash,created_at)
              VALUES (?,?,?,?,?)""",
           ("scanner-gate-01", "scanner", "Main gate",
            auth.device_token_hash(scanner_token), NOW))

for u in (uid, uid2):
    for d in ("tracker-01", "scanner-gate-01"):
        db.execute("INSERT INTO device_access (user_id,device_id) VALUES (?,?)", (u, d))

db.execute("INSERT INTO cards (card_uid,device_id,child_name) VALUES (?,?,?)",
           ("04A2B3C4", "tracker-01", "Ana"))
db.execute("INSERT INTO cards (card_uid,device_id,child_name) VALUES (?,?,?)",
           ("04D5E6F7", None, "Miguel"))

print(f"""
  Local stack seeded — {os.environ['DB_PATH']}

  Dashboard      http://localhost:8000
  Sign in        parent@example.com / {PASSWORD}
                 lola@example.com   / {PASSWORD}   (secondary contact)

  tracker/include/config.h
      #define DEVICE_TOKEN  "{tracker_token}"
      #define API_HOST      "192.168.X.X"     <- your laptop on the LAN
      #define API_PORT      8000

  scanner/include/config.h
      #define DEVICE_TOKEN  "{scanner_token}"
      #define API_BASE      "http://192.168.X.X:8000"
      static constexpr bool API_USE_TLS = false;   // plain HTTP for local dev

  Enrolled cards  04A2B3C4 (Ana, linked to tracker-01)
                  04D5E6F7 (Miguel, no tracker)
                  anything else taps as an UNKNOWN card

  Run:  uvicorn app.main:app --reload --host 0.0.0.0 --port 8000
        (--host 0.0.0.0 so the ESP32s on your LAN can reach it)
""")
