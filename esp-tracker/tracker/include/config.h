#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// SOS — the numbers here are the safety contract. Changing them changes the
// product, not just a setting. See PLAN.md 2.2.
// ---------------------------------------------------------------------------

// Hold duration before an SOS arms. A tap must never fire it: pocket false
// alarms train parents to ignore the alert.
static constexpr uint32_t SOS_HOLD_MS          = 2000;

// Grace period to abort an accidental press with a second long-hold.
static constexpr uint32_t SOS_CANCEL_WINDOW_MS = 5000;

// THE FIVE-SECOND RULE. Transmit with the best fix available at this instant,
// however poor. Indoors the GNSS branch never returns at all, so a design that
// waits for fix quality is a design that never fires inside a school.
static constexpr uint32_t SOS_TX_DEADLINE_MS   = 5000;

// Keep refining and re-sending until this, then fall back to high-rate mode.
static constexpr uint32_t SOS_REFINE_WINDOW_MS = 30000;

// After an SOS: report every 30s for 30 minutes.
static constexpr uint32_t SOS_HIGHRATE_MS      = 30000;
static constexpr uint32_t SOS_HIGHRATE_FOR_MS  = 30UL * 60UL * 1000UL;

// Device-side spam guard. Suppressed events are still logged and still raise a
// "possible stuck button" alert server-side — they are never silently dropped.
static constexpr uint8_t  SOS_MAX_PER_HOUR     = 5;

// ---------------------------------------------------------------------------
// Child-facing feedback — RGB LED (common anode, active LOW).
// See pins.h and feedback.cpp for the actual wiring/drive logic — this
// comment previously said "common cathode, active HIGH", which contradicted
// both of those and every other reference to this LED in the codebase.
// ---------------------------------------------------------------------------
// No piezo buzzer fitted. All feedback is visual via the RGB LED:
//   SOS cues  : red (armed, sent, acked, cancelled)
//   Low batt  : green tick
//   The motor is out of stock; swap is a config flag change.
static constexpr bool     FEEDBACK_USE_MOTOR   = false;   // true once stocked
static constexpr bool     FEEDBACK_USE_PIEZO   = false;   // no buzzer fitted
static constexpr uint32_t FEEDBACK_ACK_HOLD_MS = 20000;   // 0 = one-shot (motor)

// *** THINK BEFORE SETTING THIS FALSE ***
//
// A piezo is a better stand-in than an LED for most cues, because sound reaches
// a child who is not looking. But an SOS is the one case where being heard can
// be the danger: a child presses the button precisely when someone is
// frightening them, and a device that then beeps announces both the child and
// the fact that they called for help.
//
// So SOS cues stay SILENT and use the LED; the piezo carries everything else
// (boot, low battery, drill mode, "locate now" acknowledgement). This is also
// the argument for getting the vibration motor: haptics are the only channel
// that is both silent and felt.
static constexpr bool     FEEDBACK_SILENT_SOS  = true;

// ---------------------------------------------------------------------------
// Motion gate — no accelerometer in the BOM, so movement is inferred from the
// radios we already have. See motion.h for why this is arguably the better
// signal anyway: an accelerometer reports jostling, which a child fidgeting at
// a desk does all day; a changed Wi-Fi neighbourhood reports actually moving.
// ---------------------------------------------------------------------------
static constexpr uint8_t  MOTION_MAX_APS            = 12;
static constexpr float    MOTION_SIMILAR_THRESHOLD  = 0.55f;  // Jaccard; above = stationary
static constexpr uint32_t MOTION_CELL_CHECK_MS      = 60UL * 1000UL;        // tier 0, free
static constexpr uint32_t MOTION_SCAN_STATIONARY_MS = 5UL * 60UL * 1000UL;  // tier 1
static constexpr uint32_t MOTION_SCAN_MOVING_MS     = 2UL * 60UL * 1000UL;

// ---------------------------------------------------------------------------
// Routine reporting — state-driven, not timer-driven. PLAN.md 4.1.
// ---------------------------------------------------------------------------
static constexpr uint32_t CADENCE_STATIONARY_MS = 30UL * 60UL * 1000UL;
static constexpr uint32_t CADENCE_MOVING_MS     =  2UL * 60UL * 1000UL;
static constexpr uint32_t CADENCE_LOWBATT_MS    = 60UL * 60UL * 1000UL;
static constexpr uint8_t  LOWBATT_ALERT_PCT     = 20;   // alert at 20% AND again at 10%
static constexpr uint8_t  LOWBATT_CRIT_PCT      = 10;

// ---------------------------------------------------------------------------
// Position acquisition budgets (milliseconds)
// ---------------------------------------------------------------------------
static constexpr uint32_t FIX_BUDGET_BLE_MS   =  1500;
static constexpr uint32_t FIX_BUDGET_CELL_MS  =  2500;
static constexpr uint32_t FIX_BUDGET_WIFI_MS  =  4000;  // the indoor primary
static constexpr uint32_t FIX_BUDGET_GNSS_MS  = 30000;  // never completes indoors

// ---------------------------------------------------------------------------
// Network. SIM800L is 2G-only: verify AT+CREG? on the child's real route
// before building anything else (README "2G availability").
//
// Smart — this tracker and the scanner both run Smart SIMs for the
// presentation. "internet" is Smart's confirmed APN (their own SET-to-211
// auto-provisioning agrees, not just a third-party list).
// ---------------------------------------------------------------------------
#define APN            "internet"
#define APN_USER       ""
#define APN_PASS       ""

#define API_HOST       "tracker.example.net"
#define API_PORT       443
#define API_PATH_EVENTS  "/api/ingest/events"
#define DEVICE_ID      "tracker-01"
#define DEVICE_TOKEN   "myzjFRtVrfcmJ0ecEooZAYVTAWUzsaPTI9-smDrcdCk"  // bearer token for Supabase ingest
#define CLAIM_CODE     "TRACK-DEMO-2026"  // parent enters this on dashboard to pair
#define DASHBOARD_URL  "https://web-ten-beta-17.vercel.app"  // parent dashboard

// HTTP, not MQTT. Once GPRS became bursty (below), a persistent broker session
// bought nothing: no broker to run, no separate ingest worker, and the HTTP 200
// IS the server-level ack — no "QoS 1 is not confirmation" caveat to get wrong.
//
// TLS runs on the ESP32 via SSLClient, NOT on the modem: do not use the
// SIM800L's AT+HTTPSSL, it is firmware-dependent and not trustworthy for a
// child's location data.
//
// GPRS IS BROUGHT UP ON DEMAND, NOT HELD OPEN. Carrier NAT drops an idle GPRS
// TCP session after 1-5 minutes, and the keepalives needed to fight that would
// stop the modem ever reaching AT+CSCLK sleep — the biggest power lever we
// have. Bursty HTTP + SMS control (below) beats both.
static constexpr uint32_t GPRS_IDLE_CLOSE_MS = 20000;

// Batch routine positions so the 3-8s TLS handshake is amortised over many
// points instead of paid per point. Reuse the connection within a burst.
static constexpr uint16_t BATCH_MAX_EVENTS   = 60;
static constexpr uint32_t BATCH_MAX_AGE_MS   = 3UL * 60UL * 60UL * 1000UL;

// TLS certificate validation needs a roughly-correct clock, and the ESP32 has
// no RTC. Pull time from the modem (AT+CCLK? after NITZ) BEFORE the handshake,
// or every connection fails validation for no obvious reason.
static constexpr uint32_t CLOCK_MAX_SKEW_S   = 300;

// SMS is the ALWAYS-AVAILABLE channel in BOTH directions, and on 2G in the
// Philippines it is also nearly free.
//
//   UPLINK   SOS -> parent's phone directly. No GPRS, no TLS handshake, no
//            server in the path. Survives congestion, a wrong APN, and your
//            server being down.
//   DOWNLINK "Locate now" from the dashboard -> server sends SMS -> the modem
//            wakes on RI even from CSCLK sleep -> device reports. This is what
//            makes on-demand location work without holding a TCP socket open.
//
// The server de-duplicates the SMS against the HTTP event by id.
#define SOS_SMS_PRIMARY   "+639109943152"
#define SOS_SMS_SECONDARY "+639000000000"

// UPDATE (2026-08-31): "SMS as out-of-band, GPRS as the real uplink" above
// described the intended design, not a working one — this tracker's own
// modem.cpp had never actually implemented GPRS (attach()/postJson() were
// bare TODOs returning false). Rather than build that stack anyway, see
// scanner-uno/PLAN.md §1b's GPRS section first: the SAME SIM800L hardware
// on the gate scanner had its GPRS attach re-tested on real hardware and
// root-caused to the Philippines' NTC-mandated nationwide 2G/3G shutdown
// (complete by 2026-12-31) — not an APN or signal problem fixable by
// config. This tracker uses the identical 2G-only module, so it inherits
// the same ceiling. Routine location reporting (report.h) goes over SMS
// from the start, not GPRS — see SCANNER_SMS_NUMBER below. SOS and the
// "locate now" downlink above are UNCHANGED by this: they were always SMS.
//
// The scanner's own SIM800L is the relay: it already has to run its own
// modem for attendance SMS (see scanner-uno/sms_scanner/), so reusing its
// number as this tracker's report destination needs no second physical SMS
// receiver anywhere in the system. The scanner forwards whatever it
// receives to its own laptop-side dashboard (scanner-uno/dashboard/),
// which relays parseable reports on to the server — see report.h and
// server/app/tracker_sms.py for the wire format both ends must agree on
// byte for byte.
#define SCANNER_SMS_NUMBER "+639325762230"

// Runtime-configurable via BLE. Defaults above, overridden by Preferences.
extern char s_sosNumber[];
extern char s_scannerNumber[];

// Adaptive report cadence — see motion.h for the state machine driving
// this. Left at the original design's numbers even though every report is
// now a real SMS, not a free HTTP call: ~700 texts/day while moving is a
// real, accepted recurring cost for this deployment, not an oversight —
// revisit these two constants first if that cost ever needs to come down,
// nothing else about report.cpp depends on the exact values.
static constexpr uint32_t REPORT_INTERVAL_MOVING_MS     = 2UL * 60UL * 1000UL;
static constexpr uint32_t REPORT_INTERVAL_STATIONARY_MS = 30UL * 60UL * 1000UL;

// How long modem::sendSms() waits for the SIM800L's final "OK" after the
// message body — same figure as scanner-uno/sms_scanner's SMS_SEND_TIMEOUT_MS,
// confirmed reasonable against a real SMS send on that identical modem.
static constexpr uint32_t SMS_SEND_TIMEOUT_MS = 12000;

// sos.cpp's immediate parent/scanner sends use THIS shorter timeout instead
// of SMS_SEND_TIMEOUT_MS above. Reason: those two sendSms() calls run
// synchronously inline from the SOS state machine, and the whole main loop
// — including the button's cancel-hold detection — is frozen for however
// long each one blocks. At the full 12s timeout, two sequential sends
// (parent, then scanner) can freeze the loop for ~24s total: more than
// 4x SOS_CANCEL_WINDOW_MS (5000ms), meaning a slow-to-ack modem could
// silently swallow the entire cancel window. A shorter timeout here does
// not weaken delivery — a "failed" (really: still in flight) send here just
// falls through to store::push()'s persistent retry queue, which was
// already going to happen regardless of this timeout's value.
static constexpr uint32_t SOS_IMMEDIATE_SMS_TIMEOUT_MS = 4000;

// How long sos.cpp's immediate sends will wait for network registration if
// the radio was idle (CFUN=4) when SOS fired — see modem::sendSms()'s
// regTimeoutMs parameter and config.h's MODEM_CFUN_IDLE_ENABLED block.
// Short and deliberate: if registration hasn't happened by then, sendSms()
// tries the send anyway rather than blocking the SOS path indefinitely —
// the persisted store.cpp queue (see sos.cpp/store.cpp) is what actually
// guarantees delivery either way, this just bounds how long the immediate
// best-effort attempt can stall the main loop (button-cancel detection
// included) waiting on a cold radio.
static constexpr uint32_t SOS_REG_TIMEOUT_MS = 3000;

// Registration-wait bound for store::drain()'s routine (non-SOS) retries —
// Telemetry (LOC/WIFISCAN) and SOS-retry sends. Deliberately shorter than
// MODEM_REG_TIMEOUT_MS's general 15s default: drain() runs from the main
// loop gated only on sos::smsIdle(), so a routine background send blocking
// on a cold-radio registration wait ALSO blocks serviceButton() — a child
// starting to hold SOS right as a routine send begins would otherwise wait
// up to (this + SMS_SEND_TIMEOUT_MS) before the hold is even noticed, on
// top of the SOS_HOLD_MS it then still has to complete. Not as tight as
// SOS_REG_TIMEOUT_MS (routine sends can afford to wait a bit longer for a
// fair shot at registering — nothing is time-critical about them), but
// bounded well below the general default.
static constexpr uint32_t ROUTINE_REG_TIMEOUT_MS = 8000;

#define CHILD_NAME "Ana"

// Which events earn a text. Position reports never do: at the moving cadence
// that is ~700 messages a day, and attention spent on routine noise is not
// available for the SOS.
static constexpr bool     NOTIFY_SMS_ENABLED         = true;
static constexpr uint32_t NOTIFY_GEOFENCE_COOLDOWN_S = 10 * 60;   // boundary-sitting guard
static constexpr uint32_t NOTIFY_BATTERY_COOLDOWN_S  = 6 * 3600;

// Shared secret for inbound SMS commands. An unauthenticated SMS command
// channel is a remote-control interface for anyone who learns the number.
#define SMS_CMD_SECRET    "5Mz4hXMEQVVGt77MOGjJVMHglz8RaQ4h"

// Prepaid load expiry silently kills the uplink. Check balance periodically and
// surface it on the dashboard before it hits zero. Globe *143#, Smart *123#.
static constexpr uint32_t BALANCE_CHECK_MS  = 12UL * 60UL * 60UL * 1000UL;
static constexpr uint16_t BALANCE_WARN_PESOS = 20;

// Modem sleep. Without CSCLK the idle draw is 10-20mA and runtime halves.
// The modem stays ATTACHED and sleeping rather than powered down: a cold GSM
// attach costs 5-15s, and for a safety device latency beats runtime.
//
// SUPERSEDED on real hardware: modem::sleep()/wake() (CSCLK, DTR-based) are
// implemented but were never actually called from anywhere in this
// firmware — CFUN=4 idling below is what actually runs. Left defined
// rather than deleted in case a future board without the brownout problem
// wants the lighter-weight CSCLK approach instead (it keeps the modem
// attached/registered, which CFUN=4 does not — faster wake, more idle
// current).
static constexpr bool MODEM_USE_CSCLK = true;

// ---------------------------------------------------------------------------
// Modem power-cycling — LiPo BMS brownout mitigation.
//
// REAL HARDWARE PROBLEM, confirmed, not theoretical: this LiPo's BMS
// protection circuit sags/limits output enough under the SIM800L's ~2A TX
// burst to brown out the ESP32 and restart the whole device — observed
// happening after sending an SMS. A bulk cap and direct-to-LiPo wiring
// (see pins.h / AGENTS.md) reduce this but do not eliminate it if the BMS
// itself is the bottleneck; a hardware fix (better BMS, bigger cap, a
// dedicated supercap/boost stage for the modem) is the real cure. This is
// firmware mitigation, not a substitute for that hardware fix.
//
// Two-part mitigation:
//   1. Spend as little time as possible with the RF active. AT+CFUN=4
//      (radio off, AT/SIM interface still alive) is the idle state instead
//      of leaving the modem attached/registered — whatever charge reserve
//      the cap has gets to recover between deliberate, short radio-on
//      windows instead of being kept partially drained continuously.
//      modem::sendSms() wakes the radio itself if it's idle, sends, then
//      immediately drops back to CFUN=4 ("aggressive sleep fallback") —
//      every send, not just routine ones, since the brownout risk exists
//      on every TX burst regardless of what triggered it.
//   2. Never let a brownout mid-send lose the message. report.cpp's
//      routine LOC/WIFISCAN sends now go through store.cpp's NVS queue
//      instead of calling sendSms() directly — the same "persist before
//      the radio keys up" pattern the SOS path already used. If a send
//      itself crashes the board, the message survives the reboot and is
//      retried on the next window rather than silently vanishing.
static constexpr bool     MODEM_CFUN_IDLE_ENABLED = true;

// Periodic wake purely to check for INCOMING SMS (routine/queued outbound
// sends wake the radio on their own via sendSms(), independent of this
// timer — see modem::servicePowerCycle()'s own comment for how the two
// interact). Also re-checked early whenever the store queue has something
// waiting, rather than only on this fixed schedule — see servicePowerCycle().
static constexpr uint32_t MODEM_WAKE_INTERVAL_MS = 5UL * 60UL * 1000UL;
// Minimum gap between two radio-on windows even if the queue has a
// backlog — gives the cap real recovery time instead of bouncing straight
// back into CFUN=1 the instant one window closes.
static constexpr uint32_t MODEM_MIN_COOLDOWN_MS  = 30UL * 1000UL;
// How long a window stays open once registered, listening for +CMTI and
// draining the store queue, before the aggressive CFUN=4 fallback.
static constexpr uint32_t MODEM_WAKE_WINDOW_MS   = 8000;
// Give up waiting for AT+CREG to show registered after this long and fall
// back to CFUN=4 rather than holding the radio on indefinitely.
static constexpr uint32_t MODEM_REG_TIMEOUT_MS   = 15000;
// Small recovery gap between AT commands in the wake/sleep sequence, so the
// bulk cap gets a moment to recharge between exchanges rather than being
// hit with back-to-back commands right when it is most depleted.
static constexpr uint32_t MODEM_AT_GAP_MS        = 200;

// ---------------------------------------------------------------------------
// Battery — NO SENSOR IS FITTED on current hardware. There is no physical
// voltage divider on PIN_VBAT_SENSE (pins.h); that pin/ratio are reserved
// for a future board revision. Keep this false until that hardware exists —
// flipping it on THIS hardware reads a floating pin and reports meaningless
// noise as a percentage, which is worse than the honest uptime estimate it
// would replace. See battery.cpp.
static constexpr bool BATTERY_HAS_ADC_SENSOR = false;

// Uptime-estimate fallback, used while BATTERY_HAS_ADC_SENSOR is false.
// Assumes a 2500mAh LiPo with average ~5mA draw (CSCLK sleep + periodic
// scans). This is a rough estimate — real battery life depends heavily on
// usage patterns — not a substitute for a real sensor reading, just the best
// available signal without one.
static constexpr float BATTERY_CAPACITY_MAH = 2500.0f;
static constexpr float BATTERY_AVG_DRAW_MA  = 5.0f;
