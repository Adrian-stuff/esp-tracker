#pragma once
#include <stdint.h>
#include <MFRC522.h>

// RC522 UID reader — same contract as the ESP32 build's reader.h.
//
// Reads the card UID only. UIDs are trivially cloneable — fine for
// ATTENDANCE, not for access control. Do not let this scanner unlock
// anything.

struct Tap {
    char     uid[24];       // hex string
    char     id[24];        // device-generated event id; makes retries idempotent
    uint32_t recorded_at;   // from the DS3231, so an offline tap keeps honest time
    bool     sms_sent;      // this device already texted the parent about it, so
                            // the server must not send a second message
};

namespace reader {
    void begin();
    // true at most once per card per TAP_DEBOUNCE_MS. On success, the card
    // is left SELECTED (not halted) so card::read() can optionally pull
    // the offline-fallback sector from the SAME session — re-selecting a
    // card for a second read is slower and not guaranteed to work if it's
    // already being lifted away. Caller MUST call release() when done,
    // whether or not it read anything extra.
    bool poll(Tap& out);
    void release();                // PICC_HaltA + PCD_StopCrypto1 — call after every successful poll()
    bool sawDuplicate();          // last poll() was suppressed as a repeat
    MFRC522& instance();          // for card.cpp's extra sector read — nothing else should need this
}
