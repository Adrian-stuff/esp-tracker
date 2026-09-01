# Child Safety Tracker

ESP32 tracker with an SOS button, an RFID attendance scanner, and a self-hosted parent dashboard.
Built for the **Philippines** on 2G.

## Main firmwares

| Directory | Device | Hardware | What it does |
|---|---|---|---|
| **[tracker/](tracker/)** | Worn device | ESP32 + SIM800L + NEO-6M + button | SOS button, GPS/Wi-Fi/cell location, SMS + HTTP uplink |
| **[scanner/](scanner/)** | School gate | ESP32 + RC522 + DS3231 + SIM900 | RFID attendance, SMS to parent, buffered tap queue. WiFi configurable via captive portal |

Build commands:

```bash
# Tracker
pio run -t upload -e tracker && pio device monitor

# Scanner
pio run -t upload -e scanner && pio device monitor
```

## Other directories

| Directory | What it is |
|---|---|
| [scanner-uno/](scanner-uno/) | Arduino UNO variant of the scanner (legacy, alternative hardware) |
| [survey/](survey/) | Phase 0 go/no-go — run this before building anything |
| [supabase/](supabase/) | Postgres schema, RLS, pg_cron, Edge Functions — **production backend** |
| [web/](web/) | Vercel dashboard — static, Supabase Auth + Realtime |
| [server/](server/) | FastAPI + SQLite — reference implementation and local dev target |
| [relay-bridge/](relay-bridge/) | Nginx reverse proxy for SMS relay |

## Documentation

| File | What it covers |
|---|---|
| [PLAN.md](PLAN.md) | Design reasoning — read this first |
| [AGENTS.md](AGENTS.md) | Quick reference for AI agents working in this repo |
| [architecture.html](architecture.html) | System diagrams, five-second rule, escalation ladder |
| [scanner-wiring.html](scanner-wiring.html) | ESP32 scanner wiring diagram and connection table |
| [tracker-wiring.html](tracker-wiring.html) | ESP tracker wiring diagram and connection table |

## Start here

Do not build the tracker first. Run [survey/](survey/) along the child's actual route, on both
Globe and Smart, and confirm both gates pass:

1. **Uplink** — 2G registered ≥ 98%, CSQ median ≥ 15
2. **Indoor fix** — ≥ 5 Wi-Fi APs *and* the geolocation provider resolves to ≤ 50 m **inside the school**

## The three ideas the rest follows from

**Transmit at five seconds, whatever the fix looks like.** Indoors the GNSS branch never returns,
so a design that waits for fix quality is a design that never fires inside a school.

**SMS is the channel that cannot be killed.** It needs no GPRS, no TLS handshake, and no server.
SOS goes out on it; "locate now" comes back on it. HTTPS carries everything rich; SMS carries
everything that must arrive.

**Never render a position without its source, accuracy and age.** A 12 m GNSS fix and a 2 km cell
estimate are both "a pin" if you let them be. That is the most dangerous bug available here.
