#pragma once
#include <stdint.h>

// DS3231 + NTP.
//
// The RTC is not a nicety. School Wi-Fi will drop, taps keep being accepted and
// buffered, and a buffered tap carrying a millis()-derived timestamp SILENTLY
// CORRUPTS the attendance record — the worst kind of bug, because nothing looks
// wrong until someone asks when a child arrived.
//
// So: if the RTC is missing or has lost power, the scanner REFUSES to queue
// offline taps rather than recording them with a time it made up. A missing
// record is recoverable. A wrong one is not.

namespace clockw {
    bool begin();
    bool ok();               // RTC present and not flagged as having lost power
    uint32_t now();          // unix seconds, 0 if !ok()
    void syncFromNtp();      // disciplines the DS3231 whenever the network is up
    uint32_t lastSync();
}
