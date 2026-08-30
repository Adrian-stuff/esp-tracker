#pragma once
#include <stddef.h>

// Attendance record uplink — same drain-the-EEPROM-queue contract as the
// ESP32 build's net.cpp, over modem::request() instead of WiFiClient/HTTPClient.

namespace net {
    bool online();               // last-known GPRS attach state
    size_t drain();              // POST a batch from store::, commit on 200
    void refreshRosterIfStale(); // pulls the cached-hash list via modem::requestRosterStream
}
