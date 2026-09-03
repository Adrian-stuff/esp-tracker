#pragma once
#include <stdint.h>

// NEO-6M over UART1.
//
// Power-gated between fixes (~45mA while tracking), but V_BCKP stays powered so
// ephemeris survives — otherwise every fix is a 27s cold start instead of a ~1s
// hot one, which costs more energy than the gating saved.

struct GnssFix {
    bool   valid;
    double lat, lon;
    float  accuracy_m;
    float  speed_mps;
    float  heading;
    uint8_t satellites;
    uint32_t recorded_at;
};

// Snapshot of TinyGPSPlus's own parse statistics — how to tell "the module
// is alive and wired correctly, it just can't see enough sky" from "nothing
// is talking to this UART at all", without needing an actual fix. That
// distinction is the whole point of testing GPS indoors: a fix will
// legitimately never happen at a desk, but the module should still be
// producing valid (if fix-less) NMEA sentences the whole time if it is
// powered and wired correctly.
struct GnssDiag {
    uint32_t charsProcessed;    // total bytes TinyGPSPlus has run through encode()
    uint32_t passedChecksum;    // valid NMEA sentences parsed
    uint32_t failedChecksum;    // corrupt sentences — usually wrong baud or noisy wiring
    bool     satellitesValid;   // a GGA sentence has been parsed at all (even with 0 sats used)
    uint8_t  satellites;        // satellites USED IN FIX, from GGA — 0 is normal indoors
    bool     hdopValid;
    float    hdop;
    bool     hasFix;
};

namespace gps {
    void begin();
    void power(bool on);
    void off();   // power gate + drain UART buffer — frees ~45mA before SMS TX

    // Non-blocking: feed NMEA in loop(), poll for a fix.
    void service();
    bool fix(GnssFix& out);

    // Indoors this NEVER returns true. That is expected, not a bug — see
    // config.h SOS_TX_DEADLINE_MS.
    bool hasFixSince(uint32_t since_ms);

    // See GnssDiag above — this is what the "GPS" BLE/serial command uses
    // to answer "is the module actually working" without needing an
    // outdoor fix. passedChecksum > 0 means real, valid NMEA data is
    // arriving — the module and wiring are good even if satellites/hasFix
    // stay at zero/false because you're testing indoors.
    GnssDiag diagnostics();
}
