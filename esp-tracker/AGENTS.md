# AGENTS.md — AI Agent Quick Reference

## Repository structure

This is a **multi-firmware monorepo** for a child safety tracker system. Two main firmwares, one
server backend, one frontend.

```
esp-tracker/
├── tracker/          ← MAIN: ESP32 wearable (SIM800L + NEO-6M + SOS button)
├── scanner/          ← MAIN: ESP32 gate station (RC522 + DS3231 + SIM900)
├── scanner-uno/      ← Alternative: Arduino UNO variant of scanner (legacy)
├── survey/           ← Phase 0: 2G/Wi-Fi coverage survey tool
├── supabase/         ← Production backend (Postgres + Edge Functions)
├── server/           ← Local dev backend (FastAPI + SQLite)
├── web/              ← Frontend dashboard (Vercel, static)
└── relay-bridge/     ← Nginx reverse proxy for SMS relay
```

## Main firmwares

### tracker/ — ESP32 wearable device

- **Platform:** espressif32, esp32dev, Arduino framework
- **Build:** `pio run -t upload -e tracker`
- **Key files:**
  - `include/pins.h` — all GPIO assignments (authoritative source)
  - `include/config.h` — runtime constants (APN, phone numbers, timing)
  - `src/main.cpp` — setup/loop, SOS button handling
  - `src/modem.cpp` — SIM800L AT commands, power sequence
  - `src/gps.cpp` — NEO-6M UART read, TinyGPSPlus parsing
  - `src/sos.cpp` — SOS trigger/cancel state machine
  - `src/feedback.cpp` — LED + piezo cues
- **Dependencies:** TinyGPSPlus only
- **Critical:** SIM800L VCC goes directly to LiPo (3.7–4.2V), NEVER to 3.3V or 5V

### scanner/ — ESP32 school gate station

- **Platform:** espressif32, esp32dev, Arduino framework
- **Build:** `pio run -t upload -e scanner`
- **Key files:**
  - `include/pins.h` — all GPIO assignments
  - `include/config.h` — WiFi, API, SMS numbers, roster config
  - `src/main.cpp` — setup/loop, tap handling flow
  - `src/net.cpp` — WiFi connection + HTTP uplink (WiFiManager captive portal)
  - `src/reader.cpp` — RC522 RFID polling
  - `src/roster.cpp` — server-side card roster cache
  - `src/store.cpp` — offline tap queue (LittleFS)
  - `src/smsq.cpp` — SIM900 SMS queue
  - `src/lcd.cpp` — LCD display (status symbols, tap feedback)
- **Dependencies:** MFRC522, RTC, ArduinoJson, LiquidCrystal_I2C, WiFiManager
- **Critical:** RC522 is 3.3V only — 5V destroys it permanently

## Common pitfalls

1. **GPIO 12 on ESP32** — boot strapping pin; pulling it high at boot sets flash voltage and
   bricks startup. Avoid if possible.
2. **ADC2 pins** — cannot be read while Wi-Fi is active. Battery sense must use GPIO 32–39.
3. **Deep-sleep wake** — requires an RTC-capable GPIO (e.g., GPIO 33 on tracker).
4. **SIM800L power** — draws 2A TX bursts. Must be wired directly to LiPo, with 1000–2200µF
   bulk cap physically close to the module.
5. **NEO-6M V_BCKP** — must stay powered even when GPS is off, or every fix is a 27s cold start.

## Build environment

- PlatformIO Core 6.1.19
- Platform: espressif32 7.0.1
- Framework: Arduino (espressif32)
- Python: `python3 -m platformio` (installed via pip3)
- Serial port: auto-detected (usually `/dev/cu.wchusbserial*` on macOS)

## Guardrails

- Do NOT change `pins.h` without updating the corresponding wiring HTML file
- Do NOT change `config.h` values marked `change-me` without the deployer's input
- The SIM800L level shifter (1k/2.2k divider on GPIO 17) is mandatory — RXD is not 3.3V tolerant
- The scanner's `WIFI_SSID` and `WIFI_PASS` are now configurable via a captive portal
  (WiFiManager) — do not hardcode them in config.h for production deployments

## Production connectivity

- **Scanner** talks to Supabase (`https://xxxx.supabase.co`) via HTTPS. Endpoints:
  - `POST /functions/v1/ingest` — tap batches
  - `GET /functions/v1/roster` — hashed card UIDs
  - `POST /api/relay/sms` — forwards tracker SMS to the `relay-sms` Edge Function
- **Tracker** is SMS-only (2G data shutdown in PH). Sends location via SMS to scanner's SIM800L.
  Scanner relays to server. Tracker does NOT talk to Supabase directly.
- **Web dashboard** talks to Supabase directly via PostgREST + Realtime (anon key, RLS filtered).
- **`relay-sms` Edge Function** (`supabase/functions/relay-sms/`) is the bridge between the
  tracker's SMS reports and the cloud dashboard. Without it, tracker location data never reaches
  Supabase. Deploy with `supabase functions deploy relay-sms --no-verify-jwt`.
