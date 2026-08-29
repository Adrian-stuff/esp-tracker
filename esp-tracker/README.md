# Child Safety Tracker

ESP32 tracker with an SOS button, an RFID attendance scanner, and a self-hosted parent dashboard.
Built for the **Philippines** on 2G.

| | |
|---|---|
| [PLAN.md](PLAN.md) | The design and the reasoning. Read this first. |
| [architecture.html](architecture.html) | Diagrams — the system, the five-second rule, the escalation ladder |
| **[survey/](survey/)** | **Phase 0 go/no-go. Run this before anything else.** |
| [tracker/](tracker/) | ESP32 + SIM800L + NEO-6M + button — the worn device |
| [scanner/](scanner/) | ESP32 + RC522 — attendance station at the school gate |
| [supabase/](supabase/) | Postgres schema, RLS, pg_cron, Edge Functions — **production backend** |
| [web/](web/) | Vercel dashboard — static, Supabase Auth + Realtime |
| [server/](server/) | FastAPI + SQLite — reference implementation and local dev target |

## Start here

Do not build the tracker first. Run [survey/](survey/) along the child's actual route, on both
Globe and Smart, and confirm both gates pass:

1. **Uplink** — 2G registered ≥ 98%, CSQ median ≥ 15
2. **Indoor fix** — ≥ 5 Wi-Fi APs *and* the geolocation provider resolves to ≤ 50 m **inside the
   school**

Both are a weekend with the same parts the tracker uses. Both are ruinous to discover in Phase 3.

## Two backends

**Supabase + Vercel** is production: Postgres, RLS, Realtime, and the escalation ladder as
`alert_queue` rows drained by `pg_cron`. **FastAPI** is the same system in one readable Python
file — best for local development (`SMS_PROVIDER=console` exercises the whole alert ladder without
sending anything) and for understanding how the pieces fit.

They share the schema and the contracts. Before changing anything, know which one you are editing.

**Before real data reaches Supabase:** run `supabase/migrations/0002_rls.sql` and confirm every
table shows *RLS enabled*. The anon key ships in the frontend bundle; a table without RLS is
readable by anyone who opens devtools.

## The three ideas the rest follows from

**Transmit at five seconds, whatever the fix looks like.** Indoors the GNSS branch never returns,
so a design that waits for fix quality is a design that never fires inside a school.

**SMS is the channel that cannot be killed.** It needs no GPRS, no TLS handshake, and no server.
SOS goes out on it; "locate now" comes back on it. HTTPS carries everything rich; SMS carries
everything that must arrive.

**Never render a position without its source, accuracy and age.** A 12 m GNSS fix and a 2 km cell
estimate are both "a pin" if you let them be. That is the most dangerous bug available here.
