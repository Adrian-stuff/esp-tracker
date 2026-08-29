"""The escalation ladder.

The mechanism that matters here is the CUT, not the ladder. Fanning every
channel out at once and hoping is not a design: it assumes the first parent is
awake, in signal, and looking. Escalation keeps climbing until a human
explicitly acknowledges, and the acknowledgement halts it dead and records who
answered.

Without an ack endpoint you either run every rung every time — training parents
to ignore the alert — or you infer "they know" from a delivery receipt, which
proves only that a carrier accepted the message.
"""
import asyncio, time
from . import db, sms
from .config import ESCALATION
from .hub import hub

# event_id -> task, so an ack can cancel the remaining rungs.
_ladders: dict[str, asyncio.Task] = {}

def _recipients(device_id: str, order: int):
    return db.query("""SELECT u.* FROM users u
                       JOIN device_access da ON da.user_id = u.id
                       WHERE da.device_id=? AND u.escalation_order=?""",
                    (device_id, order))

def _log(sos_row_id: int, channel: str, recipient: str, res) -> None:
    db.execute("""INSERT INTO alerts (sos_event_id,channel,recipient,sent_at,
                                      provider_msg_id,delivery_status,error)
                  VALUES (?,?,?,?,?,?,?)""",
               (sos_row_id, channel, recipient, int(time.time()),
                getattr(res, "msg_id", ""), "sent" if getattr(res, "ok", True) else "failed",
                getattr(res, "error", "")))

def _still_open(event_id: str) -> bool:
    row = db.one("SELECT status FROM sos_events WHERE event_id=?", (event_id,))
    return bool(row) and row["status"] == "open"

def _message(device_id: str, loc: dict | None) -> str:
    name = db.one("SELECT child_name,name FROM devices WHERE id=?", (device_id,))
    who = (name["child_name"] or name["name"]) if name else device_id
    if loc and loc.get("lat") is not None:
        return (f"SOS from {who}. https://maps.google.com/?q={loc['lat']:.5f},{loc['lon']:.5f} "
                f"(+/-{int(loc['accuracy_m'])}m, {loc['source']})")
    return f"SOS from {who}. Position not yet known."

async def run_ladder(event_id: str, device_id: str, sos_row_id: int,
                     loc: dict | None, device_sms_sent: bool) -> None:
    started = time.time()
    for delay, channel in ESCALATION:
        wait = delay - (time.time() - started)
        if wait > 0:
            await asyncio.sleep(wait)
        if not _still_open(event_id):
            return                      # acknowledged — the ladder stops here

        text = _message(device_id, loc)

        if channel == "push":
            await hub.broadcast("sos", {"event_id": event_id, "device_id": device_id,
                                        "location": loc, "text": text})
            _log(sos_row_id, "websocket", "dashboard", None)
            # TODO: Web Push (VAPID) so a closed tab still alerts the phone.

        elif channel == "sms":
            if device_sms_sent:
                # The tracker already texted the parent directly over 2G — no
                # GPRS, no TLS, no server in the path. Sending again here would
                # make one press look like two emergencies.
                _log(sos_row_id, "sms", "skipped:device-sent", None)
                continue
            for u in _recipients(device_id, 1):
                if u["phone"]:
                    _log(sos_row_id, "sms", u["phone"], await sms.send(u["phone"], text))

        elif channel == "sms2":
            for u in _recipients(device_id, 2):
                if u["phone"]:
                    _log(sos_row_id, "sms", u["phone"], await sms.send(u["phone"], text))

        elif channel in ("voice", "voice2"):
            order = 1 if channel == "voice" else 2
            for u in _recipients(device_id, order):
                if u["phone"]:
                    # TODO: Twilio voice + TTS. Costs real money — keep it last.
                    _log(sos_row_id, "voice", u["phone"], None)

def start(event_id: str, device_id: str, sos_row_id: int,
          loc: dict | None, device_sms_sent: bool) -> None:
    task = asyncio.create_task(
        run_ladder(event_id, device_id, sos_row_id, loc, device_sms_sent))
    _ladders[event_id] = task
    task.add_done_callback(lambda _t: _ladders.pop(event_id, None))

def cancel(event_id: str) -> None:
    t = _ladders.pop(event_id, None)
    if t and not t.done():
        t.cancel()
