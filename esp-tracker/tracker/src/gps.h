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

namespace gps {
    void begin();
    void power(bool on);

    // Non-blocking: feed NMEA in loop(), poll for a fix.
    void service();
    bool fix(GnssFix& out);

    // Indoors this NEVER returns true. That is expected, not a bug — see
    // config.h SOS_TX_DEADLINE_MS.
    bool hasFixSince(uint32_t since_ms);
}
