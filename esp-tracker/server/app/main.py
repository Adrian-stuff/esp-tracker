import time, sqlite3, secrets, json, statistics, re, asyncio
from fastapi import FastAPI, Depends, HTTPException, Request, Response, WebSocket, WebSocketDisconnect
from fastapi.staticfiles import StaticFiles
from fastapi.responses import FileResponse, HTMLResponse
import pyotp

from . import db, auth, alerts, geolocate, sms, relay, attendance as att, tracker_sms
from .config import STATIC, STALE_AFTER_S, SMS_CMD_SECRET, COOKIE_SECURE, RELAY_SWEEP_S
from .hub import hub
from .models import EventBatch, TapBatch, LoginRequest, AckRequest, CreatePlace, RelayedSms

app = FastAPI(title="Child Tracker")

@app.middleware("http")
async def no_cache_static(request: Request, call_next):
    """Never cache the dashboard's own assets.

    This is a development server; a stale app.js that silently keeps running
    after an edit costs more time than the caching ever saves. Put a CDN in
    front in production if you want caching back.
    """
    response = await call_next(request)
    if request.url.path == "/" or request.url.path.startswith("/static"):
        response.headers["Cache-Control"] = "no-store, must-revalidate"
    return response

@app.on_event("startup")
async def _startup():
    db.init()

    async def sweeper():
        """Pays for anything the devices did not deliver in time.

        On the Supabase deployment this is a pg_cron job instead — a serverless
        function cannot hold a loop like this. Same table, same rules.
        """
        while True:
            await asyncio.sleep(RELAY_SWEEP_S)
            try:
                await relay.sweep()
            except Exception as e:
                print("relay sweep failed:", e, flush=True)

    asyncio.create_task(sweeper())

def _now() -> int:
    return int(time.time())

# =============================================================== ingest ======
# Device-facing. Authenticated with a per-device bearer token over HTTPS.
# The 200 response IS the ack: the device releases the event from its flash
# queue on 200 and on nothing else.

@app.post("/api/ingest/events")
async def ingest_events(batch: EventBatch, device=Depends(auth.current_device)):
    accepted, now = [], _now()

    for ev in batch.events:
        lat, lon, acc, source, place_id = ev.lat, ev.lon, ev.accuracy_m, ev.source, None

        # Unresolved Wi-Fi scan: resolve server-side, where the API key lives.
        if ev.wifi:
            aps = [a.model_dump() for a in ev.wifi]
            if lat is None:
                fix, place_id = await geolocate.resolve(device["id"], aps)
                if fix:
                    lat, lon, acc, source = fix["lat"], fix["lon"], fix["accuracy_m"], fix["source"]
            else:
                _, place_id = geolocate.match_known_place(device["id"], aps)
            # Keep the scan so the parent can name it later.
            geolocate.record_scan(device["id"], aps, ev.recorded_at, place_id)

        loc_id = None
        if lat is not None and lon is not None:
            try:
                loc_id = db.execute(
                    """INSERT INTO locations (event_id,device_id,lat,lon,accuracy_m,source,
                                              place_id,recorded_at,received_at,battery_pct,speed_mps)
                       VALUES (?,?,?,?,?,?,?,?,?,?,?)""",
                    (ev.id, device["id"], lat, lon, acc or 100.0, source or "cell",
                     place_id, ev.recorded_at, now, ev.battery_pct, ev.speed_mps))
            except sqlite3.IntegrityError:
                # Duplicate event id — a retried POST. Idempotent by design: a
                # redelivery must never become a second position.
                accepted.append(ev.id)
                continue

        if ev.kind == "sos":
            loc = ({"lat": lat, "lon": lon, "accuracy_m": acc or 0, "source": source or "cell"}
                   if lat is not None else None)
            try:
                sos_row = db.execute(
                    """INSERT INTO sos_events (event_id,device_id,triggered_at,received_at,
                                               latency_ms,first_location_id,best_location_id,
                                               device_sms_sent,is_drill)
                       VALUES (?,?,?,?,?,?,?,?,?)""",
                    (ev.id, device["id"], ev.recorded_at, now,
                     max(0, (now - ev.recorded_at) * 1000), loc_id, loc_id,
                     1 if ev.device_sms_sent else 0, 1 if ev.is_drill else 0))
                alerts.start(ev.id, device["id"], sos_row, loc, ev.device_sms_sent)
            except sqlite3.IntegrityError:
                pass   # already known; do not start a second ladder

        elif ev.kind == "health":
            db.execute("""INSERT INTO device_health (device_id,reported_at,battery_pct,
                                                     signal_csq,queue_depth)
                          VALUES (?,?,?,?,?)""",
                       (device["id"], ev.recorded_at, ev.battery_pct, ev.signal_csq, ev.queue_depth))

        accepted.append(ev.id)

        if lat is not None:
            await hub.broadcast("location", {
                "device_id": device["id"], "lat": lat, "lon": lon,
                "accuracy_m": acc, "source": source, "recorded_at": ev.recorded_at})

    last = batch.events[-1] if batch.events else None
    db.execute("""UPDATE devices SET last_seen_at=?,
                  battery_pct=COALESCE(?,battery_pct),
                  signal_csq=COALESCE(?,signal_csq),
                  balance_pesos=COALESCE(?,balance_pesos) WHERE id=?""",
               (now, last.battery_pct if last else None,
                last.signal_csq if last else None,
                last.balance_pesos if last else None, device["id"]))

    return {"accepted": accepted}

@app.post("/api/ingest/taps")
async def ingest_taps(batch: TapBatch, device=Depends(auth.current_device)):
    if device["kind"] != "scanner":
        raise HTTPException(403, "not a scanner")
    now, accepted, touched, notify_taps = _now(), [], set(), []
    for tap in batch.taps:
        card = db.one("SELECT * FROM cards WHERE card_uid=? AND active=1", (tap.card_uid,))
        try:
            db.execute("""INSERT INTO attendance (event_id,scanner_id,card_uid,child_device_id,
                                                  direction,device_sms_sent,recorded_at,received_at)
                          VALUES (?,?,?,?,?,?,?,?)""",
                       (tap.id, device["id"], tap.card_uid,
                        card["device_id"] if card else None,
                        tap.direction, 1 if tap.device_sms_sent else 0,
                        tap.recorded_at, now))
            notify_taps.append((tap, card))
            touched.add((tap.card_uid, att.local_day(tap.recorded_at)))
            await hub.broadcast("attendance", {
                "child": card["child_name"] if card else "unknown card",
                "direction": tap.direction, "recorded_at": tap.recorded_at})
        except sqlite3.IntegrityError:
            pass    # retried tap; idempotent
        accepted.append(tap.id)

    # Direction is decided HERE, not on the device — and recomputing the whole
    # day means a batch that was buffered offline and arrived hours late
    # self-corrects the taps that landed before it.
    for card_uid, day in touched:
        att.recompute(card_uid, day)

    # Tell the parent — but only about taps the scanner could not tell them
    # about itself. When the SIM900 already sent one, this is silence by design.
    for tap, card in notify_taps:
        if tap.device_sms_sent:
            continue
        row = db.one("SELECT direction FROM attendance WHERE event_id=?", (tap.id,))
        await notify_tap(tap, card, row["direction"] if row else "in")

    db.execute("UPDATE devices SET last_seen_at=? WHERE id=?", (now, device["id"]))
    return {"accepted": accepted}


@app.post("/api/relay/sms")
async def relay_sms(body: RelayedSms, device=Depends(auth.current_device)):
    """A scanner forwarding one SMS its own modem received — see
    scanner-uno/sms_scanner/src/modem.h's pollSms() and
    scanner-uno/dashboard/app.py. Handles two tracker SMS formats:

    1. LOC — routine position report (see tracker_sms.parse())
    2. WIFISCAN — WiFi BSSID scan for server-side place matching
       (see tracker_sms.parse_wifi_scan())

    Anything else is accepted (so the scanner doesn't need to guess what
    matters) but ignored.

    Authenticated as the SCANNER (its own bearer token, same as
    ingest_taps) — that proves WHICH scanner relayed this, not that the SMS
    body itself is genuine. tracker_sms.parse()'s embedded code is the
    thing that proves a tracker (not a stranger texting the scanner's
    number) actually composed this payload; the two checks are deliberately
    separate concerns.
    """
    if device["kind"] != "scanner":
        raise HTTPException(403, "not a scanner")
    now = _now()
    received_at = body.received_at or now

    # Try LOC format first
    parsed = tracker_sms.parse(body.text)
    if parsed:
        source, lat, lon, acc, recorded_at, battery_pct = parsed

        tracker = db.one("SELECT id FROM devices WHERE msisdn=? AND kind='tracker' AND active=1",
                         (body.sender,))
        if not tracker:
            return {"ok": True, "handled": False}

        event_id = f"{tracker['id']}-{recorded_at}"
        try:
            db.execute(
                """INSERT INTO locations (event_id,device_id,lat,lon,accuracy_m,source,
                                          recorded_at,received_at)
                   VALUES (?,?,?,?,?,?,?,?)""",
                (event_id, tracker["id"], lat, lon, acc, source, recorded_at, received_at))
        except sqlite3.IntegrityError:
            return {"ok": True, "handled": True, "duplicate": True}

        if battery_pct is not None:
            db.execute("UPDATE devices SET last_seen_at=?, battery_pct=? WHERE id=?",
                       (received_at, battery_pct, tracker["id"]))
        else:
            db.execute("UPDATE devices SET last_seen_at=? WHERE id=?", (received_at, tracker["id"]))
        await hub.broadcast("location", {
            "device_id": tracker["id"], "lat": lat, "lon": lon,
            "accuracy_m": acc, "source": source, "recorded_at": recorded_at,
            "battery_pct": battery_pct})
        return {"ok": True, "handled": True}

    # Try WIFISCAN format
    wifi_parsed = tracker_sms.parse_wifi_scan(body.text)
    if wifi_parsed:
        recorded_at, aps = wifi_parsed

        tracker = db.one("SELECT id FROM devices WHERE msisdn=? AND kind='tracker' AND active=1",
                         (body.sender,))
        if not tracker:
            return {"ok": True, "handled": False}

        # Match against known places
        place, place_id = geolocate.match_known_place(tracker["id"], aps)

        # Store the scan for the dashboard
        geolocate.record_scan(tracker["id"], aps, recorded_at, place_id)

        # If a known place matched, also store a location entry
        if place:
            event_id = f"{tracker['id']}-wifi-{recorded_at}"
            try:
                db.execute(
                    """INSERT INTO locations (event_id,device_id,lat,lon,accuracy_m,source,
                                              recorded_at,received_at,place_id)
                       VALUES (?,?,?,?,?,?,?,?,?)""",
                    (event_id, tracker["id"], place["lat"], place["lon"],
                     place["accuracy_m"], place["source"],
                     recorded_at, received_at, place_id))
            except sqlite3.IntegrityError:
                pass

        db.execute("UPDATE devices SET last_seen_at=? WHERE id=?", (received_at, tracker["id"]))
        await hub.broadcast("wifi_scan", {
            "device_id": tracker["id"], "recorded_at": recorded_at,
            "ap_count": len(aps), "place": place})
        return {"ok": True, "handled": True}

    return {"ok": True, "handled": False}


async def notify_tap(tap, card, direction: str) -> None:
    """Fallback notification for the Wi-Fi path.

    The scanner texts directly on its own modem, so this only fires when the
    SIM900 was unregistered or the message was dropped — which is exactly when
    the parent would otherwise hear nothing.
    """
    if not card or not card["device_id"]:
        return                                    # unknown card: an operator matter
    who = card["child_name"]
    when = time.strftime("%H:%M", time.localtime(tap.recorded_at))
    text = f"{who} tapped {direction} at {when}."
    for u in db.query("""SELECT u.phone FROM users u
                         JOIN device_access da ON da.user_id = u.id
                         WHERE da.device_id=? AND u.phone IS NOT NULL""",
                      (card["device_id"],)):
        # Offered to the devices first; the sweeper pays if nobody delivers.
        # Routine traffic only — SOS goes straight to the provider.
        relay.enqueue(u["phone"], text, child_device_id=card["device_id"], ref=tap.id)

# ================================================================ auth =======
@app.post("/api/auth/login")
async def login(body: LoginRequest, response: Response):
    user = db.one("SELECT * FROM users WHERE email=?", (body.email,))
    if not user or not auth.verify_password(user["password_hash"], body.password):
        raise HTTPException(401, "invalid credentials")
    if user["totp_secret"]:
        if not body.totp or not pyotp.TOTP(user["totp_secret"]).verify(body.totp, valid_window=1):
            raise HTTPException(401, "invalid or missing 2FA code")
    token = auth.create_session(user["id"])
    # SameSite=Lax, not Strict, and this one matters.
    #
    # Every SOS alert carries a one-tap deep link to the live map. Opening that
    # link from the Messages app is a CROSS-SITE top-level navigation, and
    # Strict withholds the cookie on exactly those — so a parent tapping the
    # link during an emergency would land on a login screen.
    #
    # Lax sends the cookie on top-level GET navigations (the deep link) while
    # still withholding it on cross-site POSTs, which is where CSRF lives.
    response.set_cookie("session", token, httponly=True, samesite="lax",
                        secure=COOKIE_SECURE, max_age=12 * 3600)
    return {"ok": True, "email": user["email"]}

@app.get("/api/auth/me")
async def whoami(request: Request):
    """Always 200. 'Are you signed in?' is a question with two valid answers,
    and probing it with an endpoint that 401s means every normal page load
    prints a red error in the console — which trains you to ignore the console
    exactly when a real 401 shows up."""
    try:
        user = await auth.current_user(request)
    except HTTPException:
        return {"authenticated": False}
    return {"authenticated": True, "email": user["email"], "role": user["role"]}

@app.post("/api/auth/logout")
async def logout(request: Request, response: Response):
    # No auth dependency: logging out when already logged out is not an error,
    # and making it one meant the sign-out button 401'd on an expired session.
    import hashlib
    tok = request.cookies.get("session", "")
    db.execute("UPDATE sessions SET revoked_at=? WHERE token_hash=?",
               (_now(), hashlib.sha256(tok.encode()).hexdigest()))
    response.delete_cookie("session")
    return {"ok": True}

# ============================================================= parent API ====
@app.get("/api/devices")
async def list_devices(request: Request, user=Depends(auth.current_user)):
    rows = db.query("""SELECT d.* FROM devices d
                       JOIN device_access da ON da.device_id = d.id
                       WHERE da.user_id=? AND d.active=1""", (user["id"],))
    out = []
    for d in rows:
        loc = db.one("""SELECT l.*, p.name AS place_name FROM locations l
                        LEFT JOIN places p ON p.id = l.place_id
                        WHERE l.device_id=? ORDER BY l.recorded_at DESC LIMIT 1""", (d["id"],))
        age = (_now() - loc["recorded_at"]) if loc else None
        out.append({
            "id": d["id"], "kind": d["kind"], "name": d["name"],
            "child_name": d["child_name"], "battery_pct": d["battery_pct"],
            "signal_csq": d["signal_csq"], "balance_pesos": d["balance_pesos"],
            "last_seen_at": d["last_seen_at"],
            # source, accuracy and age travel WITH the coordinates, always. The
            # UI cannot render an honest pin without all three.
            "location": ({"lat": loc["lat"], "lon": loc["lon"],
                          "accuracy_m": loc["accuracy_m"], "source": loc["source"],
                          "place": loc["place_name"], "recorded_at": loc["recorded_at"],
                          "age_s": age, "stale": age is not None and age > STALE_AFTER_S}
                         if loc else None)})
    auth.log_access(user, "*", "list_devices", request.client.host if request.client else "")
    return out

@app.get("/api/devices/{device_id}/history")
async def history(device_id: str, request: Request, frm: int = 0, to: int = 0,
                  limit: int = 500, user=Depends(auth.current_user)):
    auth.assert_device_access(user, device_id)
    to = to or _now()
    frm = frm or (to - 24 * 3600)
    rows = db.query("""SELECT lat,lon,accuracy_m,source,recorded_at FROM locations
                       WHERE device_id=? AND recorded_at BETWEEN ? AND ?
                       ORDER BY recorded_at ASC LIMIT ?""",
                    (device_id, frm, to, min(limit, 5000)))
    auth.log_access(user, device_id, "history", request.client.host if request.client else "")
    return [dict(r) for r in rows]

@app.post("/api/devices/{device_id}/locate")
async def locate_now(device_id: str, user=Depends(auth.current_user)):
    """On-demand fix. The server texts the device; the modem raises RI and wakes
    even from CSCLK sleep. ~10-20s end to end, and no TCP socket has to be held
    open against carrier NAT."""
    auth.assert_device_access(user, device_id)
    d = db.one("SELECT msisdn FROM devices WHERE id=?", (device_id,))
    if not d or not d["msisdn"]:
        raise HTTPException(400, "no number registered for this device")
    res = await sms.send_locate_command(d["msisdn"], SMS_CMD_SECRET)
    if not res.ok:
        raise HTTPException(502, f"could not reach the device: {res.error}")
    return {"ok": True, "eta_s": 20}

@app.get("/api/devices/{device_id}/wifi")
async def visible_wifi(device_id: str, user=Depends(auth.current_user)):
    """What the tracker can actually see — so a parent can point at it and say
    "that is school" instead of drawing a circle on a map and hoping."""
    auth.assert_device_access(user, device_id)

    named = {}
    for p in db.query("SELECT * FROM places WHERE device_id=?", (device_id,)):
        try:
            for h in json.loads(p["bssid_set"] or "[]"):
                named[h] = p["name"]
        except Exception:
            pass

    latest = db.one("""SELECT * FROM wifi_scans WHERE device_id=?
                       ORDER BY recorded_at DESC LIMIT 1""", (device_id,))
    current = []
    if latest:
        for a in json.loads(latest["aps"]):
            current.append({**a, "place": named.get(a["h"])})
        current.sort(key=lambda a: a["rssi"] or -100, reverse=True)

    # Aggregate the week: an AP seen constantly is somewhere the child spends
    # time, which is exactly what is worth naming.
    seen: dict[str, dict] = {}
    for row in db.query(
            "SELECT aps, recorded_at FROM wifi_scans WHERE device_id=? AND recorded_at > ?",
            (device_id, _now() - 7 * 86400)):
        for a in json.loads(row["aps"]):
            e = seen.setdefault(a["h"], {"h": a["h"], "ssid": a.get("ssid") or "",
                                         "rssis": [], "last": 0})
            if a.get("rssi") is not None:
                e["rssis"].append(a["rssi"])
            if a.get("ssid"):
                e["ssid"] = a["ssid"]
            e["last"] = max(e["last"], row["recorded_at"])

    # last_seen lets the UI separate "the tracker can see this right now" from
    # "it saw this on Tuesday". Those are different facts, and a parent naming
    # a place needs the first one.
    now = _now()
    frequent = sorted(
        ({"h": e["h"], "ssid": e["ssid"], "seen": len(e["rssis"]),
          "rssi": int(statistics.median(e["rssis"])) if e["rssis"] else None,
          "last_seen_s": now - e["last"] if e["last"] else None,
          "place": named.get(e["h"])} for e in seen.values()),
        key=lambda e: (-e["seen"], e["ssid"]))[:60]

    places = [dict(p) for p in db.query(
        "SELECT id,name,kind,radius_m,lat,lon FROM places WHERE device_id=?", (device_id,))]
    for p in places:
        p["ap_count"] = sum(1 for h, n in named.items() if n == p["name"])

    return {"current": current, "frequent": frequent, "places": places,
            "last_scan_at": latest["recorded_at"] if latest else None,
            "last_scan_age_s": (now - latest["recorded_at"]) if latest else None}

@app.post("/api/devices/{device_id}/places")
async def create_place(device_id: str, body: CreatePlace, user=Depends(auth.current_user)):
    """Naming a place is what turns a 30 m circle into 'At school since 08:41'.
    It also means these networks never reach the geolocation provider again."""
    auth.assert_device_access(user, device_id)
    pid = db.execute(
        """INSERT INTO places (device_id,name,kind,bssid_set,radius_m) VALUES (?,?,?,?,?)""",
        (device_id, body.name.strip(), body.kind,
         json.dumps(sorted(set(body.bssid_hashes))), body.radius_m))
    auth.log_access(user, device_id, f"create_place:{body.name}")
    return {"ok": True, "id": pid}

@app.delete("/api/places/{place_id}")
async def delete_place(place_id: int, user=Depends(auth.current_user)):
    row = db.one("SELECT device_id,name FROM places WHERE id=?", (place_id,))
    if not row:
        raise HTTPException(404, "no such place")
    auth.assert_device_access(user, row["device_id"])
    db.execute("DELETE FROM places WHERE id=?", (place_id,))
    auth.log_access(user, row["device_id"], f"delete_place:{row['name']}")
    return {"ok": True}

@app.get("/api/sos")
async def list_sos(user=Depends(auth.current_user)):
    rows = db.query("""SELECT s.*, l.lat, l.lon, l.accuracy_m, l.source
                       FROM sos_events s
                       LEFT JOIN locations l ON l.id = s.best_location_id
                       JOIN device_access da ON da.device_id = s.device_id
                       WHERE da.user_id=? AND s.triggered_at > ?
                       ORDER BY s.triggered_at DESC LIMIT 50""",
                    (user["id"], _now() - 7 * 86400))
    return [dict(r) for r in rows]

@app.post("/api/sos/{event_id}/ack")
async def ack_sos(event_id: str, body: AckRequest, user=Depends(auth.current_user)):
    row = db.one("SELECT * FROM sos_events WHERE event_id=?", (event_id,))
    if not row:
        raise HTTPException(404, "no such event")
    auth.assert_device_access(user, row["device_id"])
    db.execute("""UPDATE sos_events SET status='acknowledged', acknowledged_by=?,
                  acknowledged_at=?, notes=COALESCE(?,notes) WHERE event_id=?""",
               (user["email"], _now(), body.note, event_id))
    alerts.cancel(event_id)        # halts the ladder dead
    await hub.broadcast("sos_ack", {"event_id": event_id, "by": user["email"]})
    return {"ok": True}

@app.get("/api/attendance")
async def attendance(user=Depends(auth.current_user), limit: int = 50):
    rows = db.query("""SELECT a.*, c.child_name FROM attendance a
                       LEFT JOIN cards c ON c.card_uid = a.card_uid
                       ORDER BY a.recorded_at DESC LIMIT ?""", (min(limit, 500),))
    return [dict(r) for r in rows]

@app.get("/api/attendance/days")
async def attendance_days(user=Depends(auth.current_user), limit: int = 30):
    return att.days(limit)

# --------------------------------------------------------- device: roster ---
@app.get("/functions/v1/roster")
async def roster(device=Depends(auth.current_device)):
    """Salted hashes of enrolled card UIDs. Names never leave the server, and
    neither do the UIDs themselves.

    Lets the gate answer "is this card enrolled?" immediately without holding a
    roster a stolen scanner could reveal. The device hashes the UID it reads
    with the same salt and compares — see scanner/src/roster.h for the honest
    limits of that claim.
    """
    if device["kind"] != "scanner":
        raise HTTPException(403, "not a scanner")
    rows = db.query("SELECT card_uid FROM cards WHERE active=1")
    return {"h": [att.roster_hash(r["card_uid"]) for r in rows], "count": len(rows)}

# --------------------------------------------------------- device: outbox ---
@app.get("/functions/v1/outbox")
async def outbox_claim(device=Depends(auth.current_device), limit: int = 3):
    """Messages this device is allowed to send, under a short lease.

    A scanner may relay anything; a tracker only its own child's messages. See
    relay.py for why that asymmetry exists.
    """
    return {"messages": relay.claim(device, limit)}

@app.post("/functions/v1/outbox/{msg_id}/ack")
async def outbox_ack(msg_id: int, request: Request, device=Depends(auth.current_device)):
    body = await request.json()
    ok = relay.ack(device, msg_id, bool(body.get("sent")), body.get("error"))
    if not ok:
        raise HTTPException(409, "not claimed by this device")
    return {"ok": True}

# ------------------------------------------------------------- aliases ------
# Same paths Supabase Edge Functions serve, so moving a device between local
# dev and production is ONE constant (API_BASE) rather than a code change.
@app.post("/functions/v1/ingest")
async def ingest_alias(request: Request, device=Depends(auth.current_device)):
    body = await request.json()
    if "taps" in body:
        return await ingest_taps(TapBatch(**body), device)
    return await ingest_events(EventBatch(**body), device)

@app.get("/api/health/integrations")
async def integrations(user=Depends(auth.current_user)):
    rows = db.query("SELECT * FROM integration_health")
    devices = db.query("""SELECT d.id, d.name, d.last_seen_at, d.battery_pct, d.balance_pesos
                          FROM devices d JOIN device_access da ON da.device_id=d.id
                          WHERE da.user_id=?""", (user["id"],))
    return {"integrations": [dict(r) for r in rows],
            "devices": [dict(r) for r in devices], "now": _now()}

# =============================================================== live ========
@app.websocket("/ws/live")
async def ws_live(ws: WebSocket):
    # TODO: authenticate the socket from the session cookie before joining.
    await hub.join(ws)
    try:
        while True:
            await ws.receive_text()
    except WebSocketDisconnect:
        hub.leave(ws)

# ============================================================== static =======
app.mount("/static", StaticFiles(directory=STATIC), name="static")

@app.get("/")
async def index():
    """Stamp asset URLs with their file mtime.

    A hardcoded ?v= is worse than none: once a browser has cached that exact
    URL it serves the stale file forever, and you get a dashboard whose sign-in
    silently does nothing because it is running last week's JavaScript.
    Deriving the stamp from mtime means editing a file changes its URL.
    """
    html = (STATIC / "index.html").read_text()
    for asset in ("app.js", "style.css"):
        mtime = int((STATIC / asset).stat().st_mtime)
        html = re.sub(rf"/static/{re.escape(asset)}(\?v=\d+)?",
                      f"/static/{asset}?v={mtime}", html)
    return HTMLResponse(html, headers={"Cache-Control": "no-store, must-revalidate"})
