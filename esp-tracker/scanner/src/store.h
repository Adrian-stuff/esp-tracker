#pragma once
#include "reader.h"
#include <stddef.h>

// Offline tap buffer — a LittleFS ring, not an in-RAM array.
//
// The gate keeps accepting cards when the network is down; that is the whole
// point. Taps carry their DS3231 timestamp, so recorded_at stays honest even
// when delivery happens hours later.
//
// CONTRACT: a tap is released only on an HTTP 200. Every tap carries an id, so
// a retried batch is idempotent server-side — a redelivery must never become a
// second attendance record.

namespace store {
    bool   begin();
    bool   push(const Tap& t);              // false only when flash is full
    size_t depth();

    // Fill up to `max` taps for one batch POST. Does NOT remove them.
    size_t  peekBatch(Tap* out, size_t max);

    // Drop the first `n` taps. Call ONLY after a 200.
    void    commit(size_t n);

    // Record tap for today and return 1-based tap count (odd = in, even = out).
    uint8_t recordTap(const char* uid, uint32_t recorded_at);
}
