#pragma once
#include <stddef.h>
#include <stdint.h>

// Local audit-trail ring — EEPROM-backed, not LittleFS (the Uno has no
// filesystem). Deliberately NOT the same contract as ../src/store.h's
// queue: that one refuses push() once full because it exists to be
// drained to a server exactly once (an HTTP 200 is the ack). This build
// has no server to drain to (see ../README.md) — the SMS sent at tap time
// *is* the notification, and this ring exists only so a lost/misdelivered
// SMS is still recoverable by hand (a USB connection + the serial "DUMP"
// command, see main.cpp). So push() always succeeds, overwriting the
// oldest record once full — a full ring silently losing its OLDEST entry
// is a far smaller problem than a full ring silently refusing to log
// today's taps at all, which is what ../src/store.cpp would do here since
// nothing would ever drain it (GPRS is off — see modem.h).

namespace store {
    bool   begin();
    void   push(const char* uid, uint32_t recorded_at, bool hadCardData);
    size_t depth();

    // Walks the ring oldest-first, one record per call — used by main.cpp's
    // serial DUMP command so store.cpp stays the only module that knows the
    // EEPROM layout.
    void   forEach(void (*fn)(const char* uid, uint32_t recorded_at, bool hadCardData));

    void   clear();   // wipes the ring — for after a DUMP has been backed up by hand
}
