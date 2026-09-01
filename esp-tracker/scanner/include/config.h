#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Identity and uplink
// ---------------------------------------------------------------------------
#define DEVICE_ID     "scanner-gate-01"
#define DEVICE_SHORT  "g01"                      // prefix for generated event ids

// DEFAULT_* below only matter on first boot, or after an NVS erase — see
// settings.h. Once the config portal has saved a value, that's what runs;
// these stop being read. Change them here to change what a brand-new,
// never-configured scanner comes up as, not what this specific unit is doing.
//
// PER-DEVICE VALUES — must be set via the config portal before deployment:
//   - DEFAULT_DEVICE_TOKEN: unique per scanner, must match devices.token_hash
//   - DEFAULT_API_BASE + API_USE_TLS: Supabase URL for production
//   - ROSTER_SALT: must match the ROSTER_SALT env var in Supabase
//   - DEFAULT_SMS_PARENT_PRIMARY: parent phone number for direct SMS mode
#define DEFAULT_DEVICE_TOKEN  "K1dNVNW8Lvx9zKWcWST9TKQYO_jpczXV"        // sha256 of this is in devices.token_hash

// Local dev points at the FastAPI server on your LAN; production points at
// Supabase. The FastAPI server serves the SAME /functions/v1/* paths, so
// switching backends is one value in the config portal (or DEFAULT_API_BASE
// below, before first boot) and nothing else.
//
//   local:      "http://192.168.1.8:8000"       API_USE_TLS = false
//   production: "https://xxxx.supabase.co"      API_USE_TLS = true
#define DEFAULT_API_BASE      "https://nvdumsbxspevpvligzlw.supabase.co"
static constexpr bool API_USE_TLS = true;

// WiFi — configurable via captive portal at 192.168.4.1.
// These are defaults only; saved credentials in NVS override them.
#define AP_SSID       "Tracker-Scanner"
#define AP_PASS       ""                  // open AP; set a password if needed
#define WIFI_AP_TIMEOUT_S  180            // 3 min portal before reboot

// Fallback if no saved credentials exist (compile-time defaults).
// For production, set these via the captive portal instead.
#define WIFI_SSID     "school-wifi"
#define WIFI_PASS     "schoolpass123"

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------
// A card resting on the reader must not generate hundreds of events, but a
// legitimate re-tap minutes later must still work.
static constexpr uint32_t TAP_DEBOUNCE_MS   = 3000;

// ---------------------------------------------------------------------------
// Queue — the gate keeps accepting cards when the network is down
// ---------------------------------------------------------------------------
static constexpr uint16_t QUEUE_CAPACITY    = 2000;   // ~2 days of a busy gate
static constexpr uint32_t QUEUE_COMPACT_AT  = 500;    // rewrite the log past this
static constexpr uint8_t  BATCH_MAX         = 50;     // taps per POST, one handshake
static constexpr uint32_t DRAIN_INTERVAL_MS = 15000;

// ---------------------------------------------------------------------------
// Roster cache
// ---------------------------------------------------------------------------
// The server sends HASHES of enrolled UIDs, never names, so the gate can answer
// "is this card enrolled?" without a stolen scanner revealing who attends the
// school. See roster.h for the honest limits of that claim.
static constexpr uint16_t ROSTER_MAX_CARDS  = 400;
static constexpr uint32_t ROSTER_TTL_S      = 6UL * 3600UL;
static constexpr uint32_t ROSTER_CHECK_MS   = 10UL * 60UL * 1000UL;
#define ROSTER_SALT   "change-me-too"            // must match the roster function

// ---------------------------------------------------------------------------
// Feedback LEDs
// ---------------------------------------------------------------------------
// A 4-pin RGB LED replaces the two discretes: its R and G legs go to the SAME
// GPIOs, and blue becomes the "buffering offline" state that red used to have
// to share by blinking.
static constexpr bool LED_IS_RGB = true;

// *** GET THIS RIGHT OR EVERY CUE IS INVERTED ***
// A COMMON ANODE RGB LED has its common leg on 3V3 and lights when the GPIO is
// pulled LOW. Set this true for those. Common cathode (common leg to GND, lights
// on HIGH) is the default. Symptom of getting it wrong: all LEDs on at idle and
// dark during cues — the firmware is fine, the constant is not.
static constexpr bool LED_COMMON_ANODE = false;

// ---------------------------------------------------------------------------
// SMS (SIM900) — DIRECT mode
// ---------------------------------------------------------------------------
// The scanner texts the parent itself, so a tap is announced even when the
// school Wi-Fi is down. That is the whole reason the SIM900 is here.
//
// *** READ THIS BEFORE DEPLOYING TO A SHARED GATE ***
// Direct mode means parents' phone numbers live in flash on a box bolted to a
// wall. That is fine for ONE FAMILY at their own door, which is what this build
// is for. At a school gate serving many children it is not: it puts every
// parent's number on a stealable device and forces a firmware change whenever
// anyone enrolls or switches number — and it throws away the reason the roster
// is hashed in the first place. For that deployment, send the tap to a server
// gateway number instead and let the server fan out.
static constexpr bool SIM900_PRESENT  = true;

static constexpr bool SMS_DIRECT_MODE = true;

// Defaults only — see the DEFAULT_* note above. Set the real numbers through
// the config portal per unit; every "one family, one door" install needs its
// own, and that must never require a reflash.
#define DEFAULT_SMS_PARENT_PRIMARY   "+639171234567"
#define DEFAULT_SMS_PARENT_SECONDARY ""              // optional; "" disables

// A child fidgeting with their card must not become ten texts. The reader
// debounce is 3 s; this is the separate "do not tell the parent again" window.
static constexpr uint32_t SMS_PER_CARD_COOLDOWN_S = 5 * 60;

static constexpr uint8_t  SMS_QUEUE_DEPTH    = 8;
static constexpr uint8_t  SMS_MAX_ATTEMPTS   = 2;
static constexpr uint32_t SMS_SEND_TIMEOUT_MS = 12000;

// Unknown cards are an operator problem, not a parent one — a stranger's card
// at the gate is not news the parent can act on.
static constexpr bool SMS_ON_UNKNOWN_CARD = false;

// Relaying the server's own notifications. The scanner is mains powered and
// always attached, which makes it the best relay in the system.
static constexpr bool     RELAY_ENABLED  = true;
static constexpr uint32_t RELAY_POLL_MS  = 20000;

// ---------------------------------------------------------------------------
// LCD — I2C character LCD (PCF8574 backpack), same as the Uno build.
// Shares the I2C bus with the DS3231 at addresses 0x27 / 0x68.
// ---------------------------------------------------------------------------
#define LCD_I2C_ADDR   0x27
static constexpr uint8_t LCD_COLS = 16;
static constexpr uint8_t LCD_ROWS = 2;

// ---------------------------------------------------------------------------
// SMS inbox — polls for incoming tracker location reports
// ---------------------------------------------------------------------------
static constexpr uint32_t SMS_POLL_MS = 5000;

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------
// If the DS3231 is missing or lost power the scanner REFUSES taps rather than
// stamping them with a time it made up. A missing record is recoverable; a
// wrong one silently corrupts the register.
#define NTP_SERVER    "pool.ntp.org"
static constexpr long     TZ_OFFSET_S       = 8 * 3600;   // PH, UTC+8
static constexpr uint32_t NTP_RESYNC_S      = 24UL * 3600UL;

// ---------------------------------------------------------------------------
// Offline-fallback card (card.h) — sector 1 key. Deliberately NOT the MIFARE
// factory default (FF FF FF FF FF FF). Spells "ESPTRK" in ASCII. Must match
// card_writer's copy exactly, or writes/reads will both fail auth.
// ---------------------------------------------------------------------------
static constexpr uint8_t CARD_KEY_A[6] = {0x45, 0x53, 0x50, 0x54, 0x52, 0x4B};
