# Server — FastAPI + SQLite + Leaflet

Ingest for the tracker and the scanner, the SOS escalation ladder, and the parent dashboard.

**This is the current development target.** Supabase and Vercel are wired up in `supabase/` and
`web/` for later; nothing here depends on them.

## Run

```bash
cd server && python3 -m venv .venv && . .venv/bin/activate && pip install -r requirements.txt
python3 seed.py
uvicorn app.main:app --reload --host 0.0.0.0 --port 8000
```

`seed.py` builds a working database and prints the login plus both device tokens, ready to paste
into the firmware configs. Safe to re-run — it recreates the database.

Use `--host 0.0.0.0`, not the default, or the ESP32s on your LAN cannot reach it.

`SMS_PROVIDER=console` (the seeded default) prints messages instead of sending them, so you can
exercise the whole escalation ladder without spending a peso.

## Pointing firmware at it

The FastAPI server serves the **same `/functions/v1/*` paths as the Supabase Edge Functions**, so
moving a device between local dev and production is two constants and no code change:

```c
// scanner/include/config.h
#define API_BASE      "http://192.168.1.50:8000"   // your laptop on the LAN
static constexpr bool API_USE_TLS = false;         // plain HTTP, bench only
```

`API_USE_TLS = false` sends a child's movements in clear text. It exists so an ESP32 can talk to a
laptop without a certificate, and it must never leave your bench.

## Endpoints

| Method | Path | Who |
|---|---|---|
| POST | `/api/ingest/events` | tracker — batched events, **200 is the ack** |
| POST | `/api/ingest/taps` | scanner — batched taps |
| POST | `/functions/v1/ingest` | alias for both, matching Supabase's path |
| GET | `/functions/v1/roster` | scanner — salted hashes of enrolled cards |
| GET | `/api/attendance/days` | first in / last out per child per day |
| POST | `/api/auth/login` | parent (argon2id + optional TOTP) |
| GET | `/api/devices` | last position **with source, accuracy and age** |
| GET | `/api/devices/{id}/history` | trail points |
| POST | `/api/devices/{id}/locate` | "locate now" → SMS to the device |
| GET | `/api/sos` · POST `/api/sos/{id}/ack` | open events; ack **halts the ladder** |
| GET | `/api/attendance` | scanner taps |
| WS | `/ws/live` | live positions, SOS, taps |

## Gate attendance

**Direction is decided here, not on the device.** One reader cannot see which side of the gate a
child came from, the scanner loses per-card state on reboot, and taps buffered during a Wi-Fi
outage arrive late and out of order — a tap from 07:55 can land after one from 15:10. So
`attendance.recompute()` reorders the card's whole day by `recorded_at` and alternates in/out.
Because that is idempotent, a late batch **self-corrects** the taps that landed before it.

`app/attendance.py` is the SQLite twin of `supabase/migrations/0004_attendance.sql`. Keep them in
step, or a backend switch will silently change what the register says.

## Three things this code does deliberately

**Idempotent ingest.** Every event carries a device-generated `id` with a `UNIQUE` constraint. A
retried POST over a flaky 2G link returns 200 without inserting twice — a redelivery must never
become a second position on the map.

**One press is one alert.** The tracker texts the parent directly over 2G, bypassing GPRS, TLS and
this server entirely. When it reports `device_sms_sent: true`, the ladder skips its own t+0 SMS and
starts one rung up. Without that flag a single press arrives as two texts, and parents learn to
distrust it.

**Acknowledgement is a real cut.** `POST /api/sos/{id}/ack` cancels the running asyncio ladder, so
the voice calls at t+180 and t+300 never happen. Every attempt is recorded in `alerts` — that table
is how you find out afterwards what actually reached whom, rather than guessing.

## Before this faces the internet

Implemented: argon2id hashing, optional TOTP, per-device bearer tokens, `device_access` enforced on
every device-scoped query, an access log, HttpOnly/Secure/SameSite cookies.

Still to do — do not skip these, this is a live database of a child's location:
- Authenticate the WebSocket from the session cookie (currently open — see the TODO in `main.py`).
- Rate-limit `/api/auth/login` and the history endpoints.
- Put it behind Caddy or Traefik for TLS, and pin that certificate in the tracker firmware.
- Run the 90-day retention job, and confirm it actually runs.
- A VPS, not a port forward from a home router. Singapore is the closest region to PH.

## Bootstrapping the first parent and device

```bash
python3 - <<'PY'
import os, time, secrets, sqlite3
os.environ.setdefault("DB_PATH","./tracker.db")
from app import db, auth
db.init()
pw   = "change-this-now"
uid  = db.execute("INSERT INTO users (email,password_hash,role,phone,escalation_order,created_at)"
                  " VALUES (?,?,?,?,?,?)",
                  ("parent@example.com", auth.hash_password(pw), "parent", "+639000000000", 1, int(time.time())))
tok  = secrets.token_urlsafe(24)
db.execute("INSERT INTO devices (id,kind,name,child_name,token_hash,msisdn,created_at)"
           " VALUES (?,?,?,?,?,?,?)",
           ("tracker-01","tracker","Tracker 01","Ana", auth.device_token_hash(tok),
            "+639111111111", int(time.time())))
db.execute("INSERT INTO device_access (user_id,device_id) VALUES (?,?)", (uid,"tracker-01"))
print("login:", "parent@example.com", pw)
print("DEVICE_TOKEN for tracker/include/config.h:", tok)
PY
```

## Production vs local dev

The project has two parallel backends that serve the same API paths. Switching between them is one
`API_BASE` change in firmware — no code change needed.

| | **Local dev (FastAPI)** | **Production (Supabase + Vercel)** |
|---|---|---|
| Backend | FastAPI + SQLite on port 8000 | Supabase (Postgres + Edge Functions) |
| Frontend | Served by FastAPI at `localhost:8000` | Static files on Vercel |
| Auth | argon2id + session cookie | Supabase Auth (email/password) |
| Realtime | WebSocket at `/ws/live` (unauthenticated) | Supabase Realtime (postgres_changes) |
| SMS provider | `console` (prints to stdout) | Semaphore or Twilio |
| Device auth | Bearer token → sha256 → `devices.token_hash` | Same — identical flow |
| Ingest paths | `/functions/v1/ingest` → FastAPI handler | `/functions/v1/ingest` → Edge Function |
| Roster | `/functions/v1/roster` → FastAPI handler | `/functions/v1/roster` → Edge Function |
| Relay SMS | `/api/relay/sms` → FastAPI handler | `/api/relay/sms` → Edge Function (`relay-sms`) |
| Outbox | `/functions/v1/outbox` → FastAPI handler | `/functions/v1/outbox` → Edge Function |
| Dashboard data | WebSocket live feed + REST API | Supabase Realtime + PostgREST |

### How the backends relate

Both backends implement the same `/functions/v1/*` path structure. The scanner and tracker firmware
do not care which backend they talk to — they just POST to the configured `API_BASE`. This means:

- **Local dev**: `API_BASE = "http://192.168.1.8:8000"`, `API_USE_TLS = false`
- **Production**: `API_BASE = "https://xxxx.supabase.co"`, `API_USE_TLS = true`

The web dashboard (`web/app.js`) is hardcoded to talk to Supabase directly via PostgREST. It does
NOT go through the FastAPI server. The FastAPI server has its own dashboard at `server/static/`
that talks to the local backend.

### Relay SMS in production

The scanner forwards tracker SMS to the server via `POST /api/relay/sms`. In production, this hits
the `relay-sms` Edge Function (`supabase/functions/relay-sms/index.ts`), which:

1. Authenticates the scanner via bearer token
2. Parses the SMS text (LOC position report or WIFISCAN WiFi scan)
3. Verifies the HMAC signature
4. Stores the data in `locations` and `wifi_scans` tables
5. Returns `{ok, handled}`

This is the bridge between the tracker (SMS-only, no GPRS due to 2G shutdown) and the cloud
dashboard. Without it, tracker location data never reaches Supabase.
