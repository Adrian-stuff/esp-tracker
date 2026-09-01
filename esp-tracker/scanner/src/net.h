#pragma once
#include <cstddef>
#include "reader.h"

// Uplink to Supabase Edge Functions.
//
// CONTRACT: a tap leaves the queue only on an HTTP 200, never on "the request
// was sent". Every tap carries an id, so a retried batch is idempotent.
//
// WiFi is configurable via a captive portal (WiFiManager). On first boot or
// when credentials fail, the ESP32 starts an AP at 192.168.4.1 with a config
// form. Saved credentials persist in NVS across reboots.

namespace net {
    void begin();
    void service();          // non-blocking Wi-Fi (re)connect
    bool online();
    bool portalActive();     // true while config portal is serving

    // Drains up to BATCH_MAX taps in one connection so the TLS handshake is
    // amortised. Returns how many the server acknowledged.
    size_t drain();

    // Relays a received SMS (from the tracker) to the server's /api/relay/sms
    // endpoint. The server decides what to do with it (e.g. parse LOC reports).
    bool postRelaySms(const char* sender, const char* text);
}
