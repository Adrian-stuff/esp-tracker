"""Attendance direction — the SQLite twin of migrations/0004_attendance.sql.

The scanner does not send in/out, and should not. One reader cannot see which
side of the gate a child came from, the device loses per-card state on reboot,
and — the reason that actually forces it — taps buffered during a Wi-Fi outage
ARRIVE LATE AND OUT OF ORDER. A tap from 07:55 can land after one from 15:10.
Anything deciding direction at arrival time gets those backwards.

So direction is recomputed from the card's own history for that day, ordered by
recorded_at: odd taps are 'in', even are 'out'. Rerunning it is idempotent,
which is what makes late arrivals self-correcting.
"""
import hashlib, os

from . import db

# Must match ROSTER_SALT in scanner/include/config.h. Not a secret that protects
# against whoever holds the device — it only stops a precomputed table of every
# 4-byte MIFARE UID.
ROSTER_SALT = os.environ.get("ROSTER_SALT", "change-me-too")


def roster_hash(card_uid: str) -> str:
    """First 4 bytes of sha256(salt || uid), hex. The scanner stores ~400 of
    these in 1.6 KB of NVS and compares against what it reads."""
    return hashlib.sha256((ROSTER_SALT + card_uid).encode()).hexdigest()[:8]

# PH is UTC+8 with no DST, so a fixed offset is correct here. Revisit if this
# ever ships anywhere that observes daylight saving.
TZ_OFFSET_S = 8 * 3600


def local_day(epoch: int) -> int:
    """Day index in local time. Two taps share a day iff this matches."""
    return (epoch + TZ_OFFSET_S) // 86400


def recompute(card_uid: str, day: int) -> None:
    lo = day * 86400 - TZ_OFFSET_S
    hi = lo + 86400
    rows = db.query(
        """SELECT id, direction FROM attendance
           WHERE card_uid=? AND recorded_at >= ? AND recorded_at < ?
           ORDER BY recorded_at ASC, id ASC""",
        (card_uid, lo, hi))
    for i, r in enumerate(rows):
        want = "in" if i % 2 == 0 else "out"
        if r["direction"] != want:
            db.execute("UPDATE attendance SET direction=? WHERE id=?", (want, r["id"]))


def days(limit: int = 30) -> list[dict]:
    """One row per child per day: when they arrived, when they left.

    This is what a parent wants to see — not a list of raw scans.
    """
    rows = db.query(
        """SELECT a.card_uid, a.child_device_id, a.direction, a.recorded_at,
                  c.child_name
           FROM attendance a LEFT JOIN cards c ON c.card_uid = a.card_uid
           ORDER BY a.recorded_at DESC LIMIT ?""", (limit * 20,))
    out: dict[tuple, dict] = {}
    for r in rows:
        key = (r["card_uid"], local_day(r["recorded_at"]))
        e = out.setdefault(key, {
            "card_uid": r["card_uid"], "child_name": r["child_name"],
            "device_id": r["child_device_id"], "day": key[1],
            "first_in": None, "last_out": None, "taps": 0,
        })
        e["taps"] += 1
        if r["direction"] == "in":
            e["first_in"] = r["recorded_at"] if e["first_in"] is None \
                else min(e["first_in"], r["recorded_at"])
        else:
            e["last_out"] = r["recorded_at"] if e["last_out"] is None \
                else max(e["last_out"], r["recorded_at"])
    return sorted(out.values(), key=lambda e: e["day"], reverse=True)[:limit]
