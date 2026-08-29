"""Device-relayed SMS.

Both modems are already paid for, and on a matching PH network a device-sent
text is effectively free where Semaphore charges per message. So routine
notifications are offered to the devices first, and the paid provider becomes
the guarantee rather than the default.

Two rules hold the design together:

  1. SOS NEVER COMES HERE. An emergency must not wait for a device to poll,
     claim and confirm. alerts.py keeps sending those through the provider.

  2. A TRACKER MAY ONLY RELAY FOR ITS OWN CHILD. It already holds that parent's
     number for SOS, so relaying costs no new exposure — but a general relay
     would push other families' numbers onto a child's device. Scanners, being
     fixed and shared infrastructure, may relay anything.

The lease is what stops two devices sending the same message; the fallback
timer is what stops a dead scanner silently swallowing every notification.
"""
import time
from . import db, sms
from .config import RELAY_ENABLED, RELAY_LEASE_S, RELAY_FALLBACK_S, RELAY_MAX_ATTEMPTS


def enqueue(to_number: str, body: str, child_device_id: str | None = None,
            ref: str | None = None) -> int:
    now = int(time.time())
    # With relaying off, fallback_after is now: the sweeper pays immediately.
    fallback = now + (RELAY_FALLBACK_S if RELAY_ENABLED else 0)
    return db.execute(
        """INSERT INTO outbox (to_number, body, child_device_id, fallback_after, created_at, ref)
           VALUES (?,?,?,?,?,?)""",
        (to_number, body, child_device_id, fallback, now, ref))


def claim(device, limit: int = 3) -> list[dict]:
    """Hand a device work it is allowed to do, under a short lease."""
    if not RELAY_ENABLED:
        return []
    now = int(time.time())

    scope = "" if device["kind"] == "scanner" else " AND child_device_id = ? "
    args: tuple = (now, now)
    if device["kind"] != "scanner":
        args = (now, device["id"], now)

    rows = db.query(
        f"""SELECT id, to_number, body FROM outbox
            WHERE status IN ('pending','claimed')
              AND attempts < {RELAY_MAX_ATTEMPTS}
              AND (lease_until IS NULL OR lease_until < ?)
              AND fallback_after > ?
              {scope}
            ORDER BY created_at ASC LIMIT {int(limit)}""",
        args if device["kind"] == "scanner" else (now, now, device["id"]))

    out = []
    for r in rows:
        db.execute("""UPDATE outbox SET status='claimed', claimed_by=?, lease_until=?
                      WHERE id=? AND status IN ('pending','claimed')""",
                   (device["id"], now + RELAY_LEASE_S, r["id"]))
        out.append({"id": r["id"], "to": r["to_number"], "body": r["body"]})
    return out


def ack(device, msg_id: int, ok: bool, error: str | None = None) -> bool:
    row = db.one("SELECT * FROM outbox WHERE id=?", (msg_id,))
    if not row or row["claimed_by"] != device["id"]:
        return False
    if ok:
        db.execute("UPDATE outbox SET status='sent', sent_at=? WHERE id=?",
                   (int(time.time()), msg_id))
    else:
        # Release it rather than burning it: another device, or the provider
        # sweep, should still get this message to the parent.
        db.execute("""UPDATE outbox SET status='pending', lease_until=NULL,
                      attempts=attempts+1, error=? WHERE id=?""", (error, msg_id))
    return True


async def sweep() -> int:
    """Pay for anything the devices did not deliver in time."""
    now = int(time.time())
    due = db.query(
        """SELECT * FROM outbox
           WHERE status IN ('pending','claimed')
             AND fallback_after <= ?
           ORDER BY created_at ASC LIMIT 20""", (now,))
    sent = 0
    for m in due:
        res = await sms.send(m["to_number"], m["body"])
        db.execute("""UPDATE outbox SET status=?, sent_at=?, error=? WHERE id=?""",
                   ("provider_sent" if res.ok else "failed",
                    now if res.ok else None, None if res.ok else res.error, m["id"]))
        sent += 1 if res.ok else 0
    return sent
