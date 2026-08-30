#include "store.h"
#include "../include/config.h"
#include "../include/eeprom_layout.h"
#include <EEPROM.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

using namespace eeprom_layout;

// Classic circular buffer, one slot sacrificed to distinguish full from
// empty without a separate persisted count (one less EEPROM write per push).
static uint16_t s_head = 0;   // next slot to read
static uint16_t s_tail = 0;   // next slot to write

static int recordAddr(uint16_t slot) { return QUEUE_DATA + slot * QUEUE_RECORD_SIZE; }

static void writeRecord(uint16_t slot, const Tap& t) {
    int addr = recordAddr(slot);
    uint8_t rawLen = (uint8_t)(strlen(t.uid) / 2);
    if (rawLen > QUEUE_UID_MAX) rawLen = QUEUE_UID_MAX;   // truncate rather than overflow
    EEPROM.update(addr, rawLen);
    for (uint8_t i = 0; i < QUEUE_UID_MAX; i++) {
        uint8_t b = 0;
        if (i < rawLen) {
            char byteStr[3] = { t.uid[i * 2], t.uid[i * 2 + 1], 0 };
            b = (uint8_t)strtoul(byteStr, nullptr, 16);
        }
        EEPROM.update(addr + 1 + i, b);
    }
    EEPROM.put(addr + 1 + QUEUE_UID_MAX, t.recorded_at);
    EEPROM.update(addr + 1 + QUEUE_UID_MAX + 4, t.sms_sent ? 1 : 0);
}

static void readRecord(uint16_t slot, Tap& out) {
    int addr = recordAddr(slot);
    uint8_t rawLen = EEPROM.read(addr);
    if (rawLen > QUEUE_UID_MAX) rawLen = QUEUE_UID_MAX;
    out.uid[0] = 0;
    for (uint8_t i = 0; i < rawLen; i++) {
        uint8_t b = EEPROM.read(addr + 1 + i);
        snprintf(out.uid + i * 2, sizeof(out.uid) - i * 2, "%02X", b);
    }
    uint32_t recorded_at;
    EEPROM.get(addr + 1 + QUEUE_UID_MAX, recorded_at);
    out.recorded_at = recorded_at;
    out.sms_sent = EEPROM.read(addr + 1 + QUEUE_UID_MAX + 4) != 0;
    snprintf(out.id, sizeof out.id, "%s-%lu-%u",
             DEVICE_SHORT, (unsigned long)recorded_at, slot);
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

bool push(const Tap& t) {
    uint16_t next = (s_tail + 1) % QUEUE_CAPACITY;
    if (next == s_head) return false;   // full — one slot always kept empty
    writeRecord(s_tail, t);
    s_tail = next;
    EEPROM.put(QUEUE_TAIL, s_tail);
    return true;
}

size_t peekBatch(Tap* out, size_t max) {
    size_t n = depth();
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) readRecord((s_head + i) % QUEUE_CAPACITY, out[i]);
    return n;
}

void commit(size_t n) {
    if (n > depth()) n = depth();
    s_head = (s_head + n) % QUEUE_CAPACITY;
    EEPROM.put(QUEUE_HEAD, s_head);
}

}
