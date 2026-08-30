# dashboard — live attendance view for sms_scanner

## Why this exists

`sms_scanner` has no server ingest path (see `../../PLAN.md` §1b's GPRS section) — every
notification it sends is a direct SMS composed from what's written on the card, with no round
trip to anything. That's fine for the parent-facing notification, but it means there is no
human-readable attendance record anywhere except the scanner's own USB serial output, and the
Uno's onboard EEPROM ring (`sms_scanner/src/store.h`) only ever holds a UID + timestamp — there's
no room in 1KB for names too. This is the laptop-side piece that makes that data actually usable:
it reads the scanner's normal serial console, keeps a live table in a browser tab, and appends
every event to a local CSV so the record survives a restart.

Also forwards any SMS the scanner *receives* (see `sms_scanner/src/modem.h`'s `pollSms()`) —
useful as a lightweight two-way channel to whoever is at the gate with a laptop plugged in, though
it only works while that laptop is actually connected; there's no store-and-forward for incoming
SMS beyond the scanner's own single-message-at-a-time poll.

**Also the relay for tracker location.** The tracker (`tracker/`) has the identical dead-2G
problem as this scanner (same SIM800L, same NTC shutdown — see `../../PLAN.md` §1b), so its
routine position reports (`tracker/src/report.cpp`) go over SMS too, addressed to this scanner's
own number rather than to the server directly. This dashboard doesn't parse that format — it
relays every received SMS verbatim to the FastAPI server's `/api/relay/sms`, which decides what's
a location report (see `server/app/tracker_sms.py`) and updates Supabase/SQLite accordingly. Pass
`--server-url` to enable this; omit it and the dashboard runs exactly as before, local-only.

## Running it

```bash
cd scanner-uno/dashboard
pip install -r requirements.txt
python3 app.py
# or, to also relay tracker location SMS to the server:
python3 app.py --server-url http://localhost:8000 --server-token <this scanner's device token>
```

Then open `http://127.0.0.1:5000`. It auto-detects the scanner's serial port the same way
`../flash.sh` does (prefers `/dev/cu.wchusbserial*`); override with `--serial-port /dev/cu.xxxx`
if you have more than one matching device plugged in. If the scanner isn't plugged in yet, or the
port re-enumerates (seen on real hardware — a USB replug can rename `/dev/cu.wchusbserial<N>`),
it retries every 2 seconds rather than needing a restart.

`--server-token` is this scanner's OWN bearer token (the plaintext whose sha256 is in
`devices.token_hash` for this scanner's row) — it authenticates the dashboard as the scanner
device, proving which scanner relayed a message. It does NOT prove the SMS body itself came from a
real tracker; that's a separate check the server does against the message content itself (see
`tracker_sms.py`). Losing this token lets someone impersonate this scanner's relay traffic, same
blast radius as any other device token in this project.

## What it deliberately doesn't do

- **Binds to localhost only by default.** This page shows real names and phone numbers with no
  authentication in front of it — pass `--host 0.0.0.0` only if you specifically want it reachable
  from other devices on the same network, and understand that means anyone on that network can
  see it.
- **No database, no auth, no HTTPS.** This is a laptop-tethered pilot tool matching the scale of
  everything else in this fallback build, not a hosted service — see `../../PLAN.md`'s Security
  section for what a real deployment would need instead.
- **Doesn't persist across a different laptop.** The CSVs (`attendance_log.csv`, `sms_log.csv`)
  live next to `app.py` on whichever machine ran it — there's no sync between them if you switch
  laptops mid-pilot.
