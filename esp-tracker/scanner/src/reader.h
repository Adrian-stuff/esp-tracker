#pragma once
#include <stdint.h>

// RC522 UID reader.
//
// Reads the card UID only. UIDs are trivially cloneable with a phone or a cheap
// "magic card" — acceptable for ATTENDANCE, where the failure mode is a wrong
// record, and NOT acceptable for access control. Do not let this scanner unlock
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
    bool poll(Tap& out);          // true at most once per card per TAP_DEBOUNCE_MS
    bool sawDuplicate();          // last poll() was suppressed as a repeat
}
