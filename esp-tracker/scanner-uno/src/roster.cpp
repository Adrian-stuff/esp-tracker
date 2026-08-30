#include "roster.h"
#include "clock.h"
#include "../include/config.h"
#include "../include/eeprom_layout.h"
#include <EEPROM.h>
#include <SHA256.h>
#include <string.h>
#include <stdlib.h>

using namespace eeprom_layout;
static constexpr int EE_AT   = ROSTER_AT;
static constexpr int EE_N    = ROSTER_N;
static constexpr int EE_HASH = ROSTER_HASH;

static uint32_t s_at = 0;
static uint16_t s_n  = 0;

// 32-bit truncation of sha256(salt||uid) — MUST match the ESP32 build's
// hashUid() and the server's roster function, or every card reads unknown.
static uint32_t hashUid(const char* uid) {
    SHA256 sha;
    sha.reset();
    sha.update(ROSTER_SALT, strlen(ROSTER_SALT));
    sha.update(uid, strlen(uid));
    uint8_t out[32];
    sha.finalize(out, sizeof out);
    return ((uint32_t)out[0] << 24) | ((uint32_t)out[1] << 16) |
           ((uint32_t)out[2] << 8)  |  (uint32_t)out[3];
}

namespace roster {

bool begin() {
    EEPROM.get(EE_AT, s_at);
    EEPROM.get(EE_N, s_n);
    if (s_n > ROSTER_MAX_CARDS) { s_n = 0; s_at = 0; }   // corrupt/unwritten EEPROM
    return true;
}

bool known(const char* uid) {
    if (!s_n) return false;
    uint32_t h = hashUid(uid);
    for (uint16_t i = 0; i < s_n; i++) {
        uint32_t stored;
        EEPROM.get(EE_HASH + i * 4, stored);
        if (stored == h) return true;
    }
    return false;
}

bool stale() { return !s_at || (clockw::now() - s_at) > ROSTER_TTL_S; }

size_t   size()      { return s_n; }
uint32_t fetchedAt() { return s_at; }

// --- streaming refresh: net.cpp calls these as it reads the HTTP response
// one token at a time, so the whole JSON body never has to sit in RAM.
static uint16_t s_writeIdx = 0;

void refreshBegin() { s_writeIdx = 0; }

void refreshAdd(const char* hexHash) {
    if (s_writeIdx >= ROSTER_MAX_CARDS) return;   // more cards than this build can hold
    uint32_t h = (uint32_t)strtoul(hexHash, nullptr, 16);
    EEPROM.put(EE_HASH + s_writeIdx * 4, h);
    s_writeIdx++;
}

void refreshCommit() {
    s_n = s_writeIdx;
    // The clock, not millis(): a roster fetched before the RTC is disciplined
    // would look perpetually stale otherwise.
    s_at = clockw::now();
    EEPROM.put(EE_AT, s_at);
    EEPROM.put(EE_N, s_n);
}

}
