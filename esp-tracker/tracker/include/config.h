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
// Child-facing feedback. See feedback.h — the LED substitution is not neutral.
// ---------------------------------------------------------------------------
// A buzz is felt through a pocket; an LED has to be looked at. So the ack cue
// REPEATS, giving the child time to look down and see that their parent knows.
// Restore to a one-shot when the motor arrives.
static constexpr bool     FEEDBACK_USE_MOTOR   = false;   // true once stocked
static constexpr bool     FEEDBACK_USE_PIEZO   = true;
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
// ---------------------------------------------------------------------------
#define APN            "internet"
#define APN_USER       ""
#define APN_PASS       ""

#define API_HOST       "tracker.example.net"
#define API_PORT       443
#define API_PATH_EVENTS  "/api/ingest/events"
#define DEVICE_ID      "tracker-01"
#define DEVICE_TOKEN   "change-me"      // sent as a bearer header, per-device

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
#define SOS_SMS_PRIMARY   "+639000000000"
#define SOS_SMS_SECONDARY "+639000000000"

#define CHILD_NAME "Ana"

// Which events earn a text. Position reports never do: at the moving cadence
// that is ~700 messages a day, and attention spent on routine noise is not
// available for the SOS.
static constexpr bool     NOTIFY_SMS_ENABLED         = true;
static constexpr uint32_t NOTIFY_GEOFENCE_COOLDOWN_S = 10 * 60;   // boundary-sitting guard
static constexpr uint32_t NOTIFY_BATTERY_COOLDOWN_S  = 6 * 3600;

// Shared secret for inbound SMS commands. An unauthenticated SMS command
// channel is a remote-control interface for anyone who learns the number.
#define SMS_CMD_SECRET    "change-me"

// Prepaid load expiry silently kills the uplink. Check balance periodically and
// surface it on the dashboard before it hits zero. Globe *143#, Smart *123#.
static constexpr uint32_t BALANCE_CHECK_MS  = 12UL * 60UL * 60UL * 1000UL;
static constexpr uint16_t BALANCE_WARN_PESOS = 20;

// Modem sleep. Without CSCLK the idle draw is 10-20mA and runtime halves.
// The modem stays ATTACHED and sleeping rather than powered down: a cold GSM
// attach costs 5-15s, and for a safety device latency beats runtime.
static constexpr bool MODEM_USE_CSCLK = true;
