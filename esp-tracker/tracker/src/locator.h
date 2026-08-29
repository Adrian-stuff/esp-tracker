#pragma once
#include <stdint.h>

// Fix fusion across the four sources. Priority order is PLAN.md 3.1.
//
// The ESP32 supplies two of the four itself (Wi-Fi scan, BLE scan) at no extra
// hardware cost — which is what keeps the indoor story intact after dropping to
// a SIM800L.

enum class FixSource : uint8_t {
    None = 0,
    Gnss,        // outdoors only
    Wifi,        // THE INDOOR PRIMARY
    BleAnchor,   // known places: home, school
    Cell         // the floor: 100m - 5km
};

struct Fix {
    FixSource source;
    double    lat, lon;
    float     accuracy_m;    // must be HONEST. The UI draws this as a circle.
    uint32_t  recorded_at;
    char      place[24];     // "school" if a known place matched, else empty
};

namespace locator {
    void begin();

    // Start every source in parallel. Non-blocking.
    void beginAcquire();

    // Best fix available RIGHT NOW, however poor. Returns false only if nothing
    // at all has resolved yet.
    bool best(Fix& out);

    // A Wi-Fi scan resolved against locally-registered BSSID sets. When this
    // hits, no geolocation API is called at all: it is faster, free, and it
    // keeps the child's routine away from a third party. PLAN.md 3.3.
    //
    // Places are defined by the PARENT from the dashboard, out of the networks
    // this device actually reported seeing, and pushed back down to the device.
    bool knownPlace(Fix& out);

    void service();
}
