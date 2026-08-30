#pragma once
#include "config.h"

// Single source of truth for EEPROM offsets — roster.cpp and store.cpp each
// computing their own base address independently is exactly how two modules
// end up silently overlapping and corrupting each other's data.

namespace eeprom_layout {
    // roster.cpp: [uint32 at][uint16 n][n * uint32 hashes]
    constexpr int ROSTER_BASE = 0;
    constexpr int ROSTER_AT   = ROSTER_BASE;
    constexpr int ROSTER_N    = ROSTER_AT + 4;
    constexpr int ROSTER_HASH = ROSTER_N + 2;
    constexpr int ROSTER_END  = ROSTER_HASH + ROSTER_MAX_CARDS * 4;   // = 166 with ROSTER_MAX_CARDS=40

    // store.cpp: [uint16 head][uint16 tail][QUEUE_CAPACITY * record]
    // record = uint8 uidLen + 7 raw UID bytes (zero-padded) + uint32
    // recorded_at + uint8 flags = 13 bytes. The RAW uid, not its hash — the
    // server's ingest endpoint stores/hashes it server-side, so a
    // hash-only record here would leave net.cpp unable to rebuild the
    // exact hex string the server expects. See store.h.
    constexpr int QUEUE_UID_MAX     = 7;   // MFRC522 UIDs are 4 or 7 bytes; 7 covers both
    constexpr int QUEUE_RECORD_SIZE = 1 + QUEUE_UID_MAX + 4 + 1;   // 13
    constexpr int QUEUE_BASE = ROSTER_END;
    constexpr int QUEUE_HEAD = QUEUE_BASE;
    constexpr int QUEUE_TAIL = QUEUE_HEAD + 2;
    constexpr int QUEUE_DATA = QUEUE_TAIL + 2;
    constexpr int QUEUE_END  = QUEUE_DATA + QUEUE_CAPACITY * QUEUE_RECORD_SIZE;   // = 950 with QUEUE_CAPACITY=60

    static_assert(QUEUE_END <= 1024, "EEPROM layout exceeds the Uno's 1024-byte EEPROM");
}
