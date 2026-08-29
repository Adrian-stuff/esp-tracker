#pragma once
#include <stdint.h>
#include <stddef.h>

// Store-and-forward queue in NVS/flash.
//
// An SOS is NEVER dropped. If the modem cannot attach, the event is persisted
// with its ORIGINAL timestamp and retried with exponential backoff, forever.
// An entry is released ONLY on an HTTP 200 from the server. Because every event
// carries its own id, a retried POST is idempotent server-side — a duplicate
// delivery must not become a duplicate position.

enum class EventKind : uint8_t { Telemetry = 0, Sos = 1, Geofence = 2, Health = 3 };

struct QueuedEvent {
    char      id[24];        // generated on-device, used for the server ack
    EventKind kind;
    uint32_t  recorded_at;   // unix seconds — when it HAPPENED, not when it sent
    uint8_t   attempts;
    uint16_t  payload_len;
    char      payload[512];  // JSON
};

namespace store {
    bool begin();

    // Returns false only if flash is exhausted. SOS entries evict Telemetry
    // entries rather than being rejected themselves.
    bool push(const QueuedEvent& ev);

    // Oldest-first, except SOS events always jump the queue.
    bool peek(QueuedEvent& out);

    // Call ONLY on HTTP 200, never on "the request was sent".
    void ack(const char* id);

    size_t   depth();
    uint32_t backoff_ms(uint8_t attempts);
}
