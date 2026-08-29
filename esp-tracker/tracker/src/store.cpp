#include "store.h"
#include <Preferences.h>

// TODO: back this with NVS (Preferences) or LittleFS. NVS is simpler and
// survives OTA; LittleFS holds more. Start with NVS.

namespace store {

bool begin() { return true; }                    // TODO

bool push(const QueuedEvent& ev) { (void)ev; return true; }   // TODO

bool peek(QueuedEvent& out) { (void)out; return false; }      // TODO

void ack(const char* id) { (void)id; }           // TODO

size_t depth() { return 0; }                     // TODO

uint32_t backoff_ms(uint8_t attempts) {
    // 2s, 4s, 8s ... capped at 5 minutes. Never gives up.
    uint32_t ms = 2000UL << (attempts > 8 ? 8 : attempts);
    return ms > 300000UL ? 300000UL : ms;
}

}
