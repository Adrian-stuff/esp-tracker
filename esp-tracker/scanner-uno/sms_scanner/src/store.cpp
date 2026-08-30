#include "store.h"
#include "../include/config.h"
#include "../include/eeprom_layout.h"
#include <EEPROM.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

using namespace eeprom_layout;

// Ring buffer, one slot sacrificed to distinguish full from empty without a
// separate persisted count (one less EEPROM write per push) — same trick
// as ../src/store.cpp.
static uint16_t s_head = 0;   // oldest record
static uint16_t s_tail = 0;   // next slot to write

static int recordAddr(uint16_t slot) { return QUEUE_DATA + slot * QUEUE_RECORD_SIZE; }

static void writeRecord(uint16_t slot, const char* uid, uint32_t recorded_at, bool hadCardData) {
    int addr = recordAddr(slot);
    uint8_t rawLen = (uint8_t)(strlen(uid) / 2);
    if (rawLen > QUEUE_UID_MAX) rawLen = QUEUE_UID_MAX;   // truncate rather than overflow
    EEPROM.update(addr, rawLen);
    for (uint8_t i = 0; i < QUEUE_UID_MAX; i++) {
        uint8_t b = 0;
        if (i < rawLen) {
            char byteStr[3] = { uid[i * 2], uid[i * 2 + 1], 0 };
            b = (uint8_t)strtoul(byteStr, nullptr, 16);
        }
        EEPROM.update(addr + 1 + i, b);
    }
    EEPROM.put(addr + 1 + QUEUE_UID_MAX, recorded_at);
    EEPROM.update(addr + 1 + QUEUE_UID_MAX + 4, hadCardData ? 1 : 0);
}

static void readRecord(uint16_t slot, char* uidOut, size_t uidCap, uint32_t& atOut, bool& hadCardDataOut) {
    int addr = recordAddr(slot);
    uint8_t rawLen = EEPROM.read(addr);
    if (rawLen > QUEUE_UID_MAX) rawLen = QUEUE_UID_MAX;
    uidOut[0] = 0;
    for (uint8_t i = 0; i < rawLen && (size_t)(i * 2 + 2) < uidCap; i++) {
        uint8_t b = EEPROM.read(addr + 1 + i);
        snprintf(uidOut + i * 2, uidCap - i * 2, "%02X", b);
    }
    EEPROM.get(addr + 1 + QUEUE_UID_MAX, atOut);
    hadCardDataOut = EEPROM.read(addr + 1 + QUEUE_UID_MAX + 4) != 0;
}

namespace store {

bool begin() {
    EEPROM.get(QUEUE_HEAD, s_head);
    EEPROM.get(QUEUE_TAIL, s_tail);
    if (s_head >= QUEUE_CAPACITY || s_tail >= QUEUE_CAPACITY) { s_head = s_tail = 0; }   // unwritten EEPROM
    return true;
}

size_t depth() {
    return (s_tail >= s_head) ? (s_tail - s_head) : (QUEUE_CAPACITY - s_head + s_tail);
}

void push(const char* uid, uint32_t recorded_at, bool hadCardData) {
    writeRecord(s_tail, uid, recorded_at, hadCardData);
    uint16_t next = (s_tail + 1) % QUEUE_CAPACITY;
    if (next == s_head) s_head = (s_head + 1) % QUEUE_CAPACITY;   // full — drop the oldest, not this one
    s_tail = next;
    EEPROM.put(QUEUE_TAIL, s_tail);
    EEPROM.put(QUEUE_HEAD, s_head);
}

void forEach(void (*fn)(const char* uid, uint32_t recorded_at, bool hadCardData)) {
    char uid[16];
    uint32_t at;
    bool hadCardData;
    size_t n = depth();
    for (size_t i = 0; i < n; i++) {
        readRecord((s_head + i) % QUEUE_CAPACITY, uid, sizeof uid, at, hadCardData);
        fn(uid, at, hadCardData);
    }
}

void clear() {
    s_head = s_tail = 0;
    EEPROM.put(QUEUE_HEAD, s_head);
    EEPROM.put(QUEUE_TAIL, s_tail);
}

}
