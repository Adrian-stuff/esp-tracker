#pragma once
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Identity — no server round-trip exists in this build (see ../README.md
// for why), so this exists only to prefix locally-generated record ids
// (reader.h's Tap::id) — lets a human cross-reference a DUMP line against
// the gateway-mode builds' records if this device's cards ever get folded
// back into that roster.
// ---------------------------------------------------------------------------
#define DEVICE_ID     "scanner-gate-uno-sms"
#define DEVICE_SHORT  "gus"

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------
static constexpr uint32_t TAP_DEBOUNCE_MS = 3000;

// ---------------------------------------------------------------------------
// EEPROM — the local audit-trail ring (store.cpp). ../src/ splits its 1024
// bytes with a roster hash cache; this build has no roster (nothing can
// refresh one without GPRS — see ../README.md), so the whole EEPROM goes to
// the ring buffer instead:
//
//   4 + QUEUE_CAPACITY * 13 = 4 + 76*13 = 992 bytes (< 1024, 32 bytes spare)
//
// See include/eeprom_layout.h for the exact offsets and the static_assert
// that enforces this fits.
// ---------------------------------------------------------------------------
static constexpr uint16_t QUEUE_CAPACITY = 76;

// ---------------------------------------------------------------------------
// Feedback — character LCD (I2C, PCF8574 backpack @ 0x27). See display.h.
// ---------------------------------------------------------------------------
#define LCD_I2C_ADDR   0x27
static constexpr uint8_t LCD_COLS = 16;   // change to 20 if this is a 20x4 module
static constexpr uint8_t LCD_ROWS = 2;    // change to 4 if this is a 20x4 module

// How long a tap's result (name, "SMS Sent", an error) stays on screen
// before main.cpp reverts the LCD to Status::Idle ("Ready") on its own —
// otherwise the last tap's message just sits there until the next card,
// which reads as a frozen/unresponsive screen at an unattended gate.
static constexpr uint32_t STATUS_HOLD_MS = 4000;

// ---------------------------------------------------------------------------
// SMS — sent directly off the card's own enrolled phone number (card.h),
// never looked up from a server. No SMS_REF_MAX here: unlike ../src/'s
// gateway mode, there is no outbox job id to ack back to a server (see
// smsq.h) — one fewer field per queue slot.
// ---------------------------------------------------------------------------
static constexpr uint8_t  SMS_QUEUE_DEPTH     = 1;
static constexpr uint8_t  SMS_BODY_MAX        = 80;
static constexpr uint8_t  SMS_NUMBER_MAX      = 16;
static constexpr uint32_t SMS_SEND_TIMEOUT_MS = 12000;

// ---------------------------------------------------------------------------
// Clock — disciplined from the cellular network's NITZ time (AT+CCLK?),
// which needs only basic registration, not a GPRS/data attach (see
// modem.h). Same refusal rule as every other build in this project: no
// clock, no logged taps. A missing record is recoverable; one with a
// fabricated time silently corrupts the audit trail.
// ---------------------------------------------------------------------------
static constexpr long     TZ_OFFSET_S    = 8 * 3600;   // PH, UTC+8
static constexpr uint32_t CLOCK_RESYNC_S = 24UL * 3600UL;

// ---------------------------------------------------------------------------
// Offline card (card.h) sector key — MUST match card_writer's copy and
// ../src/include/config.h's copy exactly, byte for byte, or every card
// this reads fails auth. Spells "ESPTRK" in ASCII.
// ---------------------------------------------------------------------------
static constexpr uint8_t CARD_KEY_A[6] = {0x45, 0x53, 0x50, 0x54, 0x52, 0x4B};
