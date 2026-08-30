#pragma once
#include "reader.h"
#include <stddef.h>

// Offline tap buffer — an EEPROM ring, not LittleFS (the Uno has no
// filesystem). Same contract as the ESP32 build's store.h: a tap is
// released only on an HTTP 200, and every tap carries an id so a retried
// batch is idempotent server-side.
//
// Capacity is ~60 taps (see config.h's EEPROM layout comment) — much
// smaller than the ESP32 build's flash-backed queue. Fine for a gate with
// intermittent GPRS; not for a multi-day outage. push() returns false once
// full, same as the ESP32 build running out of flash.

namespace store {
    bool   begin();
    bool   push(const Tap& t);
    size_t depth();
    size_t peekBatch(Tap* out, size_t max);
    void   commit(size_t n);
}
