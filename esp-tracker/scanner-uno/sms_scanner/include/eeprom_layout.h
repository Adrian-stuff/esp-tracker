#pragma once
#include "config.h"

// Single source of truth for the audit-ring's EEPROM offsets — see
// store.h. Just one region in this build (no roster block — see
// config.h), unlike ../include/eeprom_layout.h's two.

namespace eeprom_layout {
    // record = uint8 uidLen + 7 raw UID bytes (zero-padded) + uint32
    // recorded_at + uint8 flags = 13 bytes. Raw UID, not a hash: this is a
    // local audit trail meant to be read by a human (store::forEach(), the
    // serial DUMP command), not matched against anything.
    constexpr int QUEUE_UID_MAX     = 7;   // MFRC522 UIDs are 4 or 7 bytes; 7 covers both
    constexpr int QUEUE_RECORD_SIZE = 1 + QUEUE_UID_MAX + 4 + 1;   // 13
    constexpr int QUEUE_HEAD = 0;
    constexpr int QUEUE_TAIL = QUEUE_HEAD + 2;
    constexpr int QUEUE_DATA = QUEUE_TAIL + 2;
    constexpr int QUEUE_END  = QUEUE_DATA + QUEUE_CAPACITY * QUEUE_RECORD_SIZE;   // 992 with QUEUE_CAPACITY=76

    static_assert(QUEUE_END <= 1024, "EEPROM layout exceeds the Uno's 1024-byte EEPROM");
}
