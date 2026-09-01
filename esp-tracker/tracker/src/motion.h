#pragma once
#include <stdint.h>

// Motion gate — WITHOUT an accelerometer.
//
// The cadence still has to be driven by state rather than a timer: a fixed
// interval wastes the modem while a child sits in a classroom for four hours,
// and is far too slow when they are in a car.
//
// The replacement is arguably better than the part it replaces, because an
// accelerometer answers the wrong question. It reports that the device is being
// jostled — which a child fidgeting at a desk does constantly — not that the
// device is somewhere else. A changed Wi-Fi neighbourhood means they actually
// went somewhere, which is the thing the cadence cares about.
//
// Three tiers, each one's cost matched to its confidence:
//
//   TIER 0  free       every 60 s   serving cell + neighbour RSSI (modem is
//                                   already awake; no extra radio time at all)
//   TIER 1  ~0.05 mAh  every 2-5 m  Wi-Fi scan, Jaccard on the BSSID set.
//                                   This tier is the authority.
//   TIER 2  expensive  on demand    GNSS — gated BY this, never part of it.
//
// Cost of losing the LIS3DH: a Wi-Fi scan every 5 min is ~13 mAh/day, about
// 0.5% of a 2500 mAh cell. The accelerometer would have drawn ~2 uA. The
// difference is lost in the noise of a SIM800L, which dominates the budget by
// three orders of magnitude.
//
// What we genuinely give up is interrupt-driven wake from deep sleep. That also
// costs little here: the modem sits at 1-3 mA in CSCLK sleep while the ESP32
// deep-sleeps at 20-40 uA, so how deeply the MCU sleeps barely moves the total.
// The real consequence is that movement is noticed up to one scan interval
// late. For a child tracker that is fine — this is not turn-by-turn navigation.

enum class MotionState : uint8_t { Unknown = 0, Stationary, Moving };

namespace motion {

// Raw WiFi scan result — BSSID, RSSI, and SSID for one access point.
struct ScanAp {
    uint8_t bssid[6];
    int8_t  rssi;
    char    ssid[24];
};

    void begin();

    // Call from loop(). Runs the tiers on their own schedules.
    void service();

    MotionState state();

    // Milliseconds since the state last changed — drives the "settled at a
    // place" and "left behind" heuristics.
    uint32_t stableFor();

    // True when there is nothing to go on: no APs visible AND no cell change
    // detected. Rural, mostly. The caller should fall back to a fixed cadence
    // rather than trusting a confident-looking "stationary".
    bool blind();

    // Last WiFi scan results — BSSID, RSSI, SSID for each visible AP.
    // Updated after each Tier 1 scan. Only valid until the next scan.
    const ScanAp* lastScan();
    uint8_t lastScanCount();
}
