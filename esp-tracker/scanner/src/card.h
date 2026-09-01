#pragma once
#include <stdint.h>

// On-card offline-fallback data — NOT the primary attendance path. The
// roster-hash check (roster.cpp) and server-driven SMS (relay.cpp) remain
// how this scanner normally works; this only gets read when the network
// is actually down and there's no way to reach the outbox relay.
//
// Wire format (48 bytes, big-endian where it matters):
//   [0]      magic       0xAC
//   [1]      version     0x01
//   [2-6]    phone_bcd   5 bytes packed BCD, 10-digit PH mobile
//   [7-8]    student_id  uint16, little-endian
//   [9-28]   name        20 bytes ASCII, zero-padded
//   [29]     crc8        over bytes [0..28]
//   [30-47]  reserved

static constexpr uint8_t CARD_MAGIC   = 0xAC;
static constexpr uint8_t CARD_VERSION = 0x01;
static constexpr uint8_t CARD_SECTOR  = 1;

struct CardData {
    char     phone[14];
    uint16_t studentId;
    char     name[21];
};

namespace card {
    bool read(CardData& out);
}
