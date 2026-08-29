#!/usr/bin/env python3
"""Fill a seeded database with one realistic school day, ending now.

    python3 seed.py && python3 mockdata.py

Positions follow home -> school -> home, and the SOURCE changes with the
environment the way it really would: GNSS on the street, Wi-Fi indoors where
GNSS is dead, and one cell-only stretch so the map has to draw an honest
kilometre-wide circle.
"""
import os, sys, time, random, json

os.environ.setdefault("DB_PATH", "./tracker.db")
os.environ.setdefault("COOKIE_SECURE", "0")
from app import db, attendance as att        # noqa: E402
db.init()
random.seed(11)

NOW = int(time.time())
TZ  = 8 * 3600                                # PH, UTC+8, no DST

# Anchor to a real PH SCHOOL DAY rather than "the last N hours". Anchoring to a
# rolling window let the morning and afternoon taps straddle PH midnight, which
# split one child's day into two rows with no in/out pair — exactly the bug the
# direction logic exists to avoid, manufactured by bad test data.
ph_midnight = ((NOW + TZ) // 86400) * 86400 - TZ
if NOW < ph_midnight + 16 * 3600:             # school day not finished yet today
    ph_midnight -= 86400                      # so use yesterday's
T0  = ph_midnight + int(6.5 * 3600)           # 06:30 PH
DAY = int(10.5 * 3600)                        # ... to 17:00 PH

HOME   = (14.65105, 121.04895)
SCHOOL = (14.63905, 121.05780)

HOME_APS = [("a0:11:22:33:44:01", "PLDTHOMEFIBR_A29"), ("a0:11:22:33:44:02", "Ate_Bhe_2.4G")]
SCHOOL_APS = [("b0:99:88:77:66:01", "DepEd-Classroom-3"),
              ("b0:99:88:77:66:02", "DepEd-Classroom-3"),
              ("b0:99:88:77:66:03", "DepEd-Admin"),
              ("b0:99:88:77:66:04", "SmartWiFi-Canteen")]

def lerp(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)

def jitter(p, m):
    return (p[0] + random.uniform(-m, m) / 111000, p[1] + random.uniform(-m, m) / 111000)

# (fraction of the window, where, source, accuracy, how many points)
PHASES = [
    (0.00, 0.06, "home",   "wifi", 22,  3),   # still at home
    (0.06, 0.14, "commute","gnss", 12, 10),   # the school run
    (0.14, 0.18, "commute","cell", 1400, 2),  # a dead spot — honest big circle
    (0.18, 0.70, "school", "wifi", 28, 14),   # the school day, GNSS useless indoors
    (0.70, 0.78, "commute","gnss", 15,  9),   # heading home
    (0.78, 1.00, "home",   "wifi", 20,  5),   # home again
]

n = 0
for f0, f1, where, source, acc, count in PHASES:
    for i in range(count):
        f = f0 + (f1 - f0) * (i / max(1, count - 1))
        ts = int(T0 + DAY * f)
        if where == "home":     pos = jitter(HOME, 25)
        elif where == "school": pos = jitter(SCHOOL, 30)
        else:
            leg = (f - f0) / max(1e-6, (f1 - f0))
            forward = f < 0.5
            pos = jitter(lerp(HOME, SCHOOL, leg if forward else 1 - leg), 12)
        batt = int(100 - 38 * f)
        db.execute("""INSERT INTO locations
              (event_id,device_id,lat,lon,accuracy_m,source,recorded_at,received_at,battery_pct,speed_mps)
              VALUES (?,?,?,?,?,?,?,?,?,?)""",
            (f"mock-{n}", "tracker-01", pos[0], pos[1],
             acc * random.uniform(.8, 1.3), source, ts, ts, batt,
             random.uniform(1.0, 4.0) if where == "commute" else 0.0))
        # Wi-Fi scans, so the "name a place" picker has something real in it.
        if source == "wifi":
            aps = HOME_APS if where == "home" else SCHOOL_APS
            db.execute("INSERT INTO wifi_scans (device_id,recorded_at,aps) VALUES (?,?,?)",
                       ("tracker-01", ts, json.dumps(
                           [{"h": f"{abs(hash(b)) % (1 << 32):08x}", "ssid": s,
                             "rssi": random.randint(-78, -48)} for b, s in aps])))
        n += 1

# A few fresh points so "last seen" reads as live rather than hours stale —
# otherwise the demo only ever shows the desaturated stale styling.
for k, back in enumerate((600, 300, 90)):
    ts = NOW - back
    pos = jitter(HOME, 20)
    db.execute("""INSERT INTO locations
          (event_id,device_id,lat,lon,accuracy_m,source,recorded_at,received_at,battery_pct,speed_mps)
          VALUES (?,?,?,?,?,?,?,?,?,?)""",
        (f"mock-live-{k}", "tracker-01", pos[0], pos[1], 21.0, "wifi", ts, ts, 62, 0.0))
    db.execute("INSERT INTO wifi_scans (device_id,recorded_at,aps) VALUES (?,?,?)",
               ("tracker-01", ts, json.dumps(
                   [{"h": f"{abs(hash(b)) % (1 << 32):08x}", "ssid": s_,
                     "rssi": random.randint(-70, -50)} for b, s_ in HOME_APS])))

db.execute("""UPDATE devices SET battery_pct=62, signal_csq=17, balance_pesos=12,
              last_seen_at=? WHERE id='tracker-01'""", (NOW - 90,))
db.execute("UPDATE devices SET last_seen_at=? WHERE id='scanner-gate-01'", (NOW - 240,))

# Gate taps. Direction is left to the server, as it must be.
# 07:52 and 15:08 PH — same PH day by construction, so the trigger produces a
# real in/out pair.
tap_in  = ph_midnight + 7 * 3600 + 52 * 60
tap_out = ph_midnight + 15 * 3600 + 8 * 60
for i, ts in enumerate((tap_in, tap_out)):
    db.execute("""INSERT INTO attendance (event_id,scanner_id,card_uid,child_device_id,recorded_at,received_at)
                  VALUES (?,?,?,?,?,?)""",
               (f"mock-tap-{i}", "scanner-gate-01", "04A2B3C4", "tracker-01", ts, ts))
att.recompute("04A2B3C4", att.local_day(tap_in))

# One place already named, one deliberately left unnamed, so both states show:
# home resolves to "At home", school is still a bare circle waiting to be named.
home_place = db.execute(
    "INSERT INTO places (device_id,name,kind,bssid_set,lat,lon,radius_m) VALUES (?,?,?,?,?,?,?)",
    ("tracker-01", "Home", "home",
     json.dumps([f"{abs(hash(b)) % (1 << 32):08x}" for b, _ in HOME_APS]),
     HOME[0], HOME[1], 30))

# Home fixes resolve to the named place, so the dashboard says "At home"
# instead of drawing a circle. School is deliberately left unnamed, so both
# states are visible side by side.
db.execute("""UPDATE locations SET place_id=? WHERE device_id='tracker-01'
              AND source='wifi' AND lat > ?""", (home_place, (HOME[0]+SCHOOL[0])/2))

import datetime as _dt
_d = _dt.datetime.fromtimestamp(ph_midnight + TZ, _dt.timezone.utc).strftime("%a %d %b")
print(f"  {n + 3} positions, 2 gate taps, Wi-Fi scans — PH school day of {_d}")
print("  'Home' is named; the school networks are left for you to name in the UI")
