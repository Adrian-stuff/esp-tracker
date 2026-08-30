#pragma once
#include <stdint.h>

// On-card offline-fallback data — NOT the primary attendance path. The
// roster-hash check (roster.cpp) and server-driven SMS (relay.cpp) remain
// how this scanner normally works; this only gets read when the network
// is actually down and there's no way to reach the outbox relay. See
// PLAN.md's card-format note for the privacy tradeoff this represents:
// a lost/stolen CARD exposes one child's contact info, not the whole
// school's roster the way a lost SCANNER would.
//
// Deliberately laid out to fit ONE MIFARE Classic sector (3 x 16-byte data
// blocks = 48 bytes) — one authentication, three sequential block reads.
// Spanning a second sector would mean a second AUTH round trip per tap,
// which is exactly the latency this format is designed to avoid. Protected
// by a project-specific key (CARD_KEY_A in config.h), not the MIFARE
// factory default — a higher bar than nothing, though MIFARE Classic's
// own cipher (Crypto1) is publicly broken and this is not real security,
// only a deterrent against casual/accidental reads with a generic reader.
//
// Wire format (48 bytes, big-endian where it matters):
//   [0]      magic       0xAC ("Attendance Card") — distinguishes a
//                         properly-written card from a blank/foreign one
//   [1]      version     0x01 — bump if this layout ever changes
//   [2-6]    phone_bcd   5 bytes packed BCD, the 10-digit PH mobile number
//                         AFTER the country code (e.g. "9171234567" for
//                         09171234567) — firmware prepends "+63" when
//                         sending. Packed BCD, not ASCII, because ASCII
//                         digits would need 10 bytes for the same data;
//                         this is the single biggest saving in the layout.
//   [7-8]    student_id  uint16, little-endian — the durable, exact-match
//                         identifier; the name field exists for the SMS
//                         text, not for matching.
//   [9-28]   name        20 bytes ASCII, zero-padded/truncated — first
//                         name only, matching notify.cpp's existing
//                         "Ana tapped in" style messages elsewhere in
//                         this project.
//   [29]     crc8        over bytes [0..28] — a corrupted read must never
//                         result in texting a wrong or garbled number;
//                         this is the fallback path's only integrity check
//                         since there's no server round-trip to catch it.
//   [30-47]  reserved    zero-filled, for future fields without a layout
//                         version bump

static constexpr uint8_t CARD_MAGIC   = 0xAC;
static constexpr uint8_t CARD_VERSION = 0x01;
static constexpr uint8_t CARD_SECTOR  = 1;   // sector 0 holds the manufacturer block, skip it

struct CardData {
    char     phone[14];     // "+63" (3) + 10 digits + NUL = 14, ready to hand straight to smsq::enqueue()
    uint16_t studentId;
    char     name[21];      // 20 chars + NUL
};

namespace card {
    // Reads CARD_SECTOR from the card ALREADY SELECTED by reader::poll()
    // (must be called before PICC_HaltA — see main.cpp's fallback path).
    // Returns false on any auth failure, CRC mismatch, or bad magic/version
    // — all treated the same way: "no usable offline data", not a crash.
    bool read(CardData& out);
}
