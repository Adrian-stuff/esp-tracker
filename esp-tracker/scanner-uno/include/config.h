#pragma once
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Identity and uplink
// ---------------------------------------------------------------------------
#define DEVICE_ID     "scanner-gate-uno-01"
#define DEVICE_SHORT  "gu1"                      // prefix for generated event ids
#define DEVICE_TOKEN  "change-me"                // sha256 of this is in devices.token_hash

// *** THIS SIM800L CANNOT SPEAK TLS. *** Real TLS needs BearSSL/mbedTLS-class
// RAM (multiple KB) that a 2 KB AVR does not have — the same reason the
// tracker's own SIM800L terminates TLS on the ESP32 instead of the modem
// (modem.h) rather than trusting AT+CIPSSL. There is no equivalent headroom
// here to do it on-MCU.
//
// So these must point at a PLAIN HTTP endpoint, not Supabase directly — see
// ../relay-bridge/, a small nginx reverse proxy that does exactly this and
// holds no secrets (Supabase's Edge Functions validate DEVICE_TOKEN and
// hold their own service-role access; this device's traffic passes through
// unchanged). It needs a public IP or domain — the SIM800L reaches it over
// cellular data, not your LAN — see relay-bridge/README.md for why "just
// run it locally" doesn't work here and what does.
//
// Split into host/port rather than one URL string: there is no HTTPClient
// here to parse one, and modem.cpp talks to the SIM800L in terms of
// AT+CIPSTART's own host/port arguments anyway.
// Temporary: ngrok TCP tunnel for demo testing, forwarding to relay-bridge
// (currently pointed at httpbin.org as a stand-in until a real Supabase
// project is up). Free-tier ngrok TCP addresses are randomly assigned and
// change on tunnel restart — update this if the tunnel is regenerated.
// DIAGNOSTIC: raw IP (neverssl.com's, resolved externally), bypassing the
// SIM800L's own DNS resolution entirely — testing whether DNS is the
// actual blocker rather than the port.
#define API_HOST      "34.223.124.45"
static constexpr uint16_t API_PORT = 80;

// Smart — both the tracker and this scanner run Smart SIMs for the
// presentation. Confirmed against Smart's own auto-provisioning (text SET
// to 211) as well as independent APN references, not just guessed. If this
// ever needs to run on Globe instead: "internet.globe.com.ph" (postpaid) or
// "http.globe.com.ph" (some prepaid plans still expect the older value).
#define GPRS_APN  "internet"
#define GPRS_USER ""
#define GPRS_PASS ""

// GPRS attach re-tested against real hardware (2026-08-31), then root-caused
// for good — this is a CLOSED question now, not an open one:
//
//   1. AT+CGATT=1 fails immediately, before APN/CSTT is even reached —
//      already ruled out as a config bug (modem was confirmed registered,
//      AT+CREG stat=1, with real if weak signal, AT+CSQ ~3).
//   2. The SIM's data plan itself is NOT the blocker — mobile data was
//      confirmed working on a phone with this same SIM.
//   3. That phone's network-mode settings don't even offer a 2G-only
//      option anymore.
//   4. The Philippines' NTC has a MANDATED nationwide 2G/3G shutdown,
//      area-by-area, complete by 2026-12-31 (Smart already dropped 3G in
//      Sept 2025) — see PLAN.md §1b's GPRS section for sources.
//
// The SIM800L is 2G-ONLY hardware — no 3G/4G fallback exists for it to use
// instead. All four findings point the same way: 2G data is being (or has
// been) switched off in this area as a matter of national policy, not a
// bug anywhere in this codebase. GPRS_ENABLED stays false PERMANENTLY for
// this hardware — re-enabling it is not a "try again later" situation.
// SMS (AT+CMGS/CMGL, plain network registration, not GPRS) is unaffected
// and is the durable answer for this module, not a stopgap — see
// sms_scanner/, which is the long-term firmware for this board. A working
// HTTP/ingest path on this hardware would require different cellular
// hardware entirely (a 3G/4G-capable module), not a firmware change.
static constexpr bool GPRS_ENABLED = false;

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------
static constexpr uint32_t TAP_DEBOUNCE_MS   = 3000;

// ---------------------------------------------------------------------------
// EEPROM layout — the Uno has exactly 1024 bytes total, shared between the
// roster cache and the offline tap queue. Both are much smaller than the
// ESP32 build's LittleFS-backed versions; that is the real capacity ceiling
// of this fallback. See PLAN.md §1b for the external-EEPROM upgrade path if
// a real school population needs more than this.
//
//   roster:  6 + ROSTER_MAX_CARDS * 4 =  6 + 40*4  =  166 bytes
//   queue:   4 + QUEUE_CAPACITY * 13  =  4 + 60*13 =  784 bytes  (uid is
//            stored RAW, not hashed — the server needs the actual UID
//            string, so this is bigger than the roster's hash-only slots)
//   ------------------------------------------------
//   total                                            950 bytes  (< 1024)
//
// See include/eeprom_layout.h for the exact byte offsets — it enforces this
// with a static_assert so the two modules can never silently overlap.
// ---------------------------------------------------------------------------
static constexpr uint16_t ROSTER_MAX_CARDS  = 40;
static constexpr uint32_t ROSTER_TTL_S      = 6UL * 3600UL;
static constexpr uint32_t ROSTER_CHECK_MS   = 10UL * 60UL * 1000UL;
#define ROSTER_SALT   "change-me-too"            // must match the roster function AND the ESP32 build

static constexpr uint16_t QUEUE_CAPACITY    = 60;     // buffered taps, offline
static constexpr uint32_t DRAIN_INTERVAL_MS = 20000;

// The ESP32 build batches 10 taps per POST because ArduinoJson streams the
// request body straight to the TLS socket. Here the whole JSON body has to
// exist as one string in RAM first — modem::request() needs Content-Length
// before it can send a byte of the request — so this has to stay small.
//
// These numbers were tuned against `pio run -e uno`'s actual reported RAM,
// not estimated. Re-tightened again after adding the LiquidCrystal_I2C
// library (see display.cpp): that pushed static RAM from 73% to 79.3%,
// leaving only 423 bytes free — uncomfortably close to net::drain()'s own
// ~326-byte peak stack usage at the OLD BATCH_MAX=2/JSON_BODY_CAP=220
// settings, the exact class of margin-too-thin problem that turned out to
// be the real cause of the self-test's reset loop (see PLAN.md §1b
// Status). BATCH_MAX=1/JSON_BODY_CAP=180 restores real headroom. Re-check
// with `pio run -e uno` after changing either.
static constexpr uint8_t  BATCH_MAX      = 1;
static constexpr size_t   JSON_BODY_CAP  = 180;

// ---------------------------------------------------------------------------
// Feedback — character LCD (I2C), not an RGB LED. The module on hand turned
// out to be a PCF8574 LCD backpack (confirmed by a live I2C scan: it
// answers at 0x27, not the 0x62 originally assumed for a PCA9633 RGB
// driver) — its 8 GPIO lines are hard-wired to LCD control signals
// (RS/RW/E/backlight/D4-D7), not free outputs, so it can't drive an RGB LED
// at all. Status feedback is short text instead of a colour. See display.h.
// ---------------------------------------------------------------------------
#define LCD_I2C_ADDR   0x27
static constexpr uint8_t LCD_COLS = 16;   // change to 20 if this is a 20x4 module
static constexpr uint8_t LCD_ROWS = 2;    // change to 4 if this is a 20x4 module

// ---------------------------------------------------------------------------
// SMS / attendance notification — GATEWAY mode by default
// ---------------------------------------------------------------------------
// Unlike the ESP32 scanner's SMS_DIRECT_MODE=true default (which hardcodes
// ONE family's number in flash, and is explicitly documented there as unfit
// for a real multi-child gate — see scanner/include/config.h), this build
// defaults to gateway mode: the SERVER decides who gets texted and what it
// says, based on the tap it already received, and writes that job to the
// `outbox` table. This device only relays already-composed messages,
// polled on its own timer (RELAY_POLL_MS), decoupled from tap timing —
// there is no per-tap "look up this card's parent number" network call
// anywhere in this design. That decoupling IS the latency optimization:
// a tap gets feedback the instant reader::poll() returns, using only the
// LOCAL roster hash cache, regardless of what the modem is doing.
static constexpr bool     RELAY_ENABLED  = true;
static constexpr uint32_t RELAY_POLL_MS  = 20000;

// A REAL regression vs the ESP32 build, not just a smaller number: the
// ESP32's SMS queue holds 8 messages of up to 151 chars each because it can
// afford to. If the server's outbox composer writes a longer message, this
// device truncates it; keep outbox message bodies short by design if
// targeting this build.
//
// QUEUE_DEPTH=1, not 2: this was the single largest remaining RAM cost in
// the whole firmware (~218 bytes for 2 slots — the smsq Job struct is
// SMS_NUMBER_MAX+SMS_BODY_MAX+SMS_REF_MAX+padding per slot). Safe to cut
// to one because SMS here is explicitly best-effort, never the durable
// record (see smsq.h) — a second message queuing up just waits one more
// RELAY_POLL_MS cycle rather than being lost, the same tradeoff already
// made for BATCH_MAX after two separate RAM-margin crashes this build.
static constexpr uint8_t  SMS_QUEUE_DEPTH     = 1;
static constexpr uint8_t  SMS_BODY_MAX        = 80;
static constexpr uint8_t  SMS_NUMBER_MAX      = 16;
static constexpr uint8_t  SMS_REF_MAX         = 12;
static constexpr uint32_t SMS_SEND_TIMEOUT_MS = 12000;

// ---------------------------------------------------------------------------
// Clock — the Uno has no Wi-Fi/NTP path, so it disciplines the DS3231 from
// the cellular network's NITZ time (AT+CCLK?) instead. Same refusal rule as
// the ESP32 build: no clock, no queued taps. A missing record is
// recoverable; a record with a fabricated time silently corrupts it.
// ---------------------------------------------------------------------------
static constexpr long     TZ_OFFSET_S   = 8 * 3600;   // PH, UTC+8
static constexpr uint32_t CLOCK_RESYNC_S = 24UL * 3600UL;

// ---------------------------------------------------------------------------
// Offline-fallback card (card.h) — sector 1 key. Deliberately NOT the MIFARE
// factory default (FF FF FF FF FF FF), which every off-the-shelf reader
// tries first — this is only a deterrent against casual/generic reads, not
// real security (MIFARE Classic's Crypto1 cipher is publicly broken). Spells
// "ESPTRK" in ASCII so it's easy to recognize in a hex dump; change this and
// re-write every card in the field if it's ever compromised. Must match
// card_writer's copy exactly, or writes/reads will both fail auth.
// ---------------------------------------------------------------------------
static constexpr uint8_t CARD_KEY_A[6] = {0x45, 0x53, 0x50, 0x54, 0x52, 0x4B};
