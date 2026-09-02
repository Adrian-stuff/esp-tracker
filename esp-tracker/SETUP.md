# Child Tracker — Setup Guide

## Architecture at a glance

```
┌──────────┐    RFID     ┌──────────┐  WiFi/HTTPS  ┌──────────┐
│ Card AAA │────────────▶│ Scanner  │─────────────▶│ Supabase │
│ Card BBB │             │ gate-01  │              │ (cloud)  │
└──────────┘             └──────────┘              └────┬─────┘
                                                        │
                                        ┌───────────────┤
                                        │               │
                                   ┌────▼─────┐   ┌────▼─────┐
                                   │ Dashboard│   │ SMS relay│
                                   │ (parent) │   │          │
                                   └────┬─────┘   └────┬─────┘
                                        │              │
                              claim code│         SMS  │
                                        │              │
                                   ┌────▼─────┐   ┌───▼──────┐
                                   │ device_  │   │ Tracker  │
                                   │ access   │   │ (child)  │
                                   └──────────┘   └──────────┘
```

## Three things that must be connected

| Layer | What | Where stored | How to create |
|-------|------|-------------|---------------|
| Card → Child | `card_uid` maps to a child's name | `cards` table | Card writer tool or manual SQL |
| Child → Parent | Parent can see their child's device | `device_access` table | Claim code or manual SQL |
| Tracker ↔ Scanner | SMS number for location relay | Tracker NVS + `devices.token_hash` | BLE command or SMS command |

---

## Step 1: Supabase setup

### 1a. Run migrations

All 6 migrations should already be applied. Verify:

```bash
supabase migration list --linked
```

Expected:
```
0001  | 0001
0002  | 0002
0003  | 0003
0004  | 0004
0005  | 0005
0006  | 0006
```

### 1b. Set Edge Function secrets

```bash
supabase secrets set \
  ROSTER_SALT=7c59IwikdA0xsaMjJaQ8Kw \
  SMS_CMD_SECRET=5Mz4hXMEQVVGt77MOGjJVMHglz8RaQ4h
```

### 1c. Deploy Edge Functions

```bash
supabase functions deploy relay-sms --no-verify-jwt
supabase functions deploy dispatch --no-verify-jwt
supabase functions deploy ingest --no-verify-jwt
supabase functions deploy roster --no-verify-jwt
supabase functions deploy outbox --no-verify-jwt
supabase functions deploy claim --no-verify-jwt
supabase functions deploy locate --no-verify-jwt
supabase functions deploy sweep-outbox --no-verify-jwt
```

### 1d. Insert demo data

Paste into Supabase SQL Editor:

```sql
-- Devices
INSERT INTO devices (id, kind, name, token_hash, active, claim_code)
VALUES
  ('scanner-gate-01', 'scanner', 'School Gate Scanner',
   'c7e67e5a2d6b0609c1c5356f869da6ac81cb6386b6e37073ec38732db120ebb8',
   true, 'GATE-DEMO-2026'),
  ('tracker-01', 'tracker', 'Child Tracker',
   'f386e10dd4f066100bc7b33a38b64bf47cef5e0d4840774bad7f9ef786aec3e1',
   true, 'TRACK-DEMO-2026')
ON CONFLICT (id) DO UPDATE SET claim_code = EXCLUDED.claim_code;

-- Demo RFID cards
INSERT INTO cards (card_uid, device_id, child_name, active)
VALUES
  ('AA:BB:CC:DD', 'tracker-01', 'Maria Santos', true),
  ('11:22:33:44', 'tracker-01', 'Juan Dela Cruz', true)
ON CONFLICT (card_uid) DO NOTHING;
```

---

## Step 2: Flash firmware

### Tracker (ESP32 + SIM800L + NEO-6M)

```bash
cd esp-tracker
python3 -m platformio run -t upload -e tracker
```

Key config values (`tracker/include/config.h`):

| Setting | Value | Notes |
|---------|-------|-------|
| `DEVICE_ID` | `tracker-01` | Must match `devices.id` in Supabase |
| `DEVICE_TOKEN` | `myzjFRtVrfcmJ0ecEooZAYVTAWUzsaPTI9-smDrcdCk` | Must match `devices.token_hash` (sha256) |
| `SOS_SMS_PRIMARY` | Your phone number | Where SOS alerts go |
| `SCANNER_SMS_NUMBER` | Scanner's SIM number | Where LOC/WIFISCAN SMS go |
| `SMS_CMD_SECRET` | `5Mz4hXMEQVVGt77MOGjJVMHglz8RaQ4h` | Must match Supabase env var |

### Scanner (ESP32 + RC522 + SIM900)

```bash
python3 -m platformio run -t upload -e scanner
```

Key config values (`scanner/include/config.h`):

| Setting | Value | Notes |
|---------|-------|-------|
| `DEVICE_ID` | `scanner-gate-01` | Must match `devices.id` |
| `DEFAULT_DEVICE_TOKEN` | `K1dNVNW8Lvx9zKWcWST9TKQYO_jpczXV` | Must match `devices.token_hash` |
| `ROSTER_SALT` | `7c59IwikdA0xsaMjJaQ8Kw` | Must match Supabase env var |
| `DEFAULT_API_BASE` | `https://nvdumsbxspevpvligzlw.supabase.co` | Your Supabase project URL |

---

## Step 3: Connect scanner to WiFi

1. Power on the scanner
2. Connect phone to WiFi network **"Tracker-Scanner"** (open, no password)
3. Browser opens captive portal at `192.168.4.1`
4. Select your school WiFi, enter password
5. Scanner saves credentials and connects

---

## Step 4: Configure tracker SMS numbers

### Option A: WiFi captive portal (recommended)

1. Power on the tracker while **holding the SOS button** (LED blinks blue 3x = setup mode)
2. Connect phone to WiFi **"Tracker-Setup"** (open, no password)
3. Browser opens to `192.168.4.1` — enter SOS number, scanner number, child's name
4. Tap "Save & reboot"

> Config portal times out after 5 minutes and auto-reboots.

### Option B: BLE (phone app)

1. Open nRF Connect or LightBlue
2. Scan for **"ESP-Tracker"**
3. Connect, open FFF2 characteristic
4. Send: `SCANNER +639325762230` (scanner's SIM number)
5. Send: `SOS +639109943152` (parent's phone number)
6. Send: `WIFI` to enter config mode remotely
7. Send: `STATUS` to verify

### Option C: SMS (any phone)

Text the tracker's SIM number:

```
5Mz4hXMEQVVGt77MOGjJVMHglz8RaQ4h SCANNER +639325762230
5Mz4hXMEQVVGt77MOGjJVMHglz8RaQ4h SOS +639109943152
```

The tracker replies confirming each change.

---

## Step 5: Register cards

### Option A: Card writer tool (recommended)

```bash
pip install pyserial requests
cd scanner-uno/card_writer
python3 card_writer_gui.py
```

1. Connect Arduino Uno via USB
2. Select port, click Connect
3. Fill in: parent phone, student ID, student name
4. Click "Write Card" — hold card on reader
5. Card is written AND registered in Supabase automatically

### Option B: Manual SQL

```sql
INSERT INTO cards (card_uid, device_id, child_name, active)
VALUES ('AA:BB:CC:DD', 'tracker-01', 'Maria Santos', true);
```

### Option C: Read card UID first

Use the card writer's "Read / Verify Card" button to get the UID, then insert via SQL.

---

## Step 6: Parent pairs their account

1. Open the dashboard (Vercel URL or `localhost:8000`)
2. Sign up with email + password
3. Enter claim code: **`TRACK-DEMO-2026`**
4. Dashboard now shows the tracker on the map

---

## How data flows

### Card tap → Dashboard attendance

```
Card taps RC522 → scanner queues tap → WiFi available →
POST /functions/v1/ingest → ingest function upserts attendance →
trigger recomputes direction (odd=in, even=out) →
dashboard shows via Realtime
```

### Tracker position → Dashboard map

```
Tracker motion detects movement →
LOC SMS to scanner (GPS coordinates) + WIFISCAN SMS (BSSIDs) →
scanner relays to relay-sms →
LOC upserts into locations table →
WIFISCAN matches BSSIDs against named places →
dashboard shows via Realtime
```

### SOS button → Dashboard alert

```
Parent holds SOS 2s → tracker sends SMS to BOTH parent + scanner →
relay-sms inserts sos_events + alert_queue →
dashboard shows SOS banner via Realtime →
dispatch cron sends escalation SMS →
parent acknowledges → ladder stops
```

### Dashboard "Locate now" → Tracker responds

```
Dashboard POST /functions/v1/locate →
locate function sends "LOCATE <secret>" SMS to tracker →
tracker wakes, gets GPS fix, sends LOC SMS back →
relay-sms stores location → dashboard updates
```

---

## Claim codes

| Device | Code | Purpose |
|--------|------|---------|
| Tracker | `TRACK-DEMO-2026` | Parent enters this on dashboard to pair |
| Scanner | `GATE-DEMO-2BBC` | Pre-existing, for scanner pairing |

---

## Configuring the tracker via SMS

Send to the tracker's SIM number:

```
<secret> SOS +639123456789          — set SOS number
<secret> SCANNER +639325762230      — set scanner relay number
```

Current secret: `5Mz4hXMEQVVGt77MOGjJVMHglz8RaQ4h`

The tracker replies confirming each change. Deletes the command SMS after processing.

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| WiFi config portal won't open | Go to `192.168.4.1` manually. Ensure connected to "Tracker-Setup" |
| Tracker won't enter setup mode | Hold SOS button BEFORE releasing power. LED must blink blue 3 times |
| Scanner shows "No RTC" | Check DS1302 wiring (3-wire, NOT I2C). Pins: CLK=14, DAT=15, CE=2 |
| Scanner can't connect to WiFi | Reset via `RST` button, connect to "Tracker-Scanner" AP |
| Scanner upload fails (HTTP -1) | Check `ROSTER_SALT` matches Supabase env var |
| Tracker has no signal | SIM800L VCC must go DIRECTLY to LiPo (3.7-4.2V), not via ESP32 |
| Tracker SMS not received | Check `SCANNER_SMS_NUMBER` is correct, SIM has load |
| Dashboard shows no devices | Run the demo data SQL, then sign in and enter claim code |
| Attendance not appearing | Cards must be registered in `cards` table with matching `card_uid` |
| SOS not reaching dashboard | Ensure scanner SIM900 is online and relaying SMS |
| WiFi scans not resolving | Named places must exist in `places` table with matching BSSID hashes |

---

## Security notes

- `SMS_CMD_SECRET` prevents unauthorized SMS reconfiguration of the tracker
- `ROSTER_SALT` prevents roster enumeration (card UIDs are hashed)
- Device tokens are per-device SHA-256 hashes (one compromised device can't affect others)
- RLS on all tables — dashboard shows only paired devices
- `setInsecure()` on scanner HTTPS (no cert pinning) — acceptable for a school project
- MIFARE Classic UIDs are cloneable — card security is a deterrent, not encryption
