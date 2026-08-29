#pragma once
#include "reader.h"

// Uplink to Supabase Edge Functions.
//
// CONTRACT: a tap leaves the queue only on an HTTP 200, never on "the request
// was sent". Every tap carries an id, so a retried batch is idempotent.

namespace net {
    void begin();
    void service();          // non-blocking Wi-Fi (re)connect
    bool online();

    // Drains up to BATCH_MAX taps in one connection so the TLS handshake is
    // amortised. Returns how many the server acknowledged.
    size_t drain();
}
