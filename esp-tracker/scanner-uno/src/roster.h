#pragma once
#include <stdint.h>
#include <stddef.h>

// EEPROM-backed roster cache — same wire format and hash algorithm as the
// ESP32 build's roster.cpp (32-bit truncation of sha256(salt||uid)), so the
// server's /functions/v1/roster response is byte-for-byte reusable by
// either MCU. Capacity is far smaller here: see config.h's EEPROM layout
// comment for why.

namespace roster {
    bool begin();
    bool known(const char* uid);
    bool stale();
    size_t   size();
    uint32_t fetchedAt();

    // Parses one hex hash out of a roster response, called once per line by
    // net.cpp's streaming reader — the Uno does not have RAM to buffer the
    // whole JSON response before parsing it, unlike the ESP32's ArduinoJson
    // pull-parser.
    void refreshBegin();
    void refreshAdd(const char* hexHash);
    void refreshCommit();
}
