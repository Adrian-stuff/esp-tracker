#pragma once
#include <stdint.h>
#include <stddef.h>

// Store-and-forward queue in NVS/flash.
//
// An SOS is NEVER dropped. If the modem cannot attach, the event is
// persisted with its ORIGINAL timestamp and retried with exponential
// backoff, forever. An entry is released ONLY on real end-to-end
// confirmation the event reached the server — not merely on "the request
// was sent" — because every event carries its own id, making a retried
// delivery idempotent server-side rather than a duplicate position.
//
// HOW THE ACK ACTUALLY GETS BACK HERE (round-trip, implemented):
// sos.cpp appends " ID:<local id>" to the SCANNER-bound copy only (never
// the parent's human-readable text). relay-sms uses that id both to dedupe
// a retried SOS against the same button press (sos_events.event_id is
// UNIQUE — a retry hits the conflict path and does not start a second
// escalation ladder) and to enqueue an outbox row addressed back to this
// device's own msisdn: "<SMS_CMD_SECRET> ACK <id>". The scanner's existing
// outbox relay (scanner/src/relay.cpp) delivers that with zero scanner
// firmware changes — it already sends whatever outbox gives it to whatever
// to_number is specified. modem::pollSmsCommand()'s onAck callback receives
// it here and calls sos::onServerAck(), which is what finally calls
// store::ack() below and plays Cue::Acked ("your parent has been told").
//
// Only the SOS path has this. Telemetry (LOC/WIFISCAN) events still ack on
// local modem send success (drain() below) — a weaker guarantee, but an
// acceptable one for routine reports where the next report a few minutes
// later supersedes a lost one anyway; it isn't for a safety event.

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
    //
    // ev is IN/OUT: on success, ev.id is overwritten with the generated id,
    // so the caller can later store::ack(ev.id) directly — e.g. sos.cpp uses
    // this to cancel the queued retry copy once its own immediate send
    // already succeeded, rather than both paths independently delivering
    // the same SOS.
    bool push(QueuedEvent& ev);

    // Oldest-first, except SOS events always jump the queue.
    bool peek(QueuedEvent& out);

    // Contract: call ONLY on confirmed end-to-end delivery, never on "the
    // request was sent" — enforced for SOS entries (see store.h's file
    // header / sos::onServerAck()). Telemetry entries are the one
    // exception: drain() below still acks those on local modem send
    // success, a deliberately weaker guarantee for routine reports — see
    // the file header's last paragraph for why that's an acceptable
    // tradeoff there but not for SOS.
    void ack(const char* id);

    size_t   depth();
    uint32_t backoff_ms(uint8_t attempts);

    // Drain the queue by sending events via SMS. Tries one event per call,
    // respects backoff, and acks on success. Non-blocking — call from loop().
    void drain();
}
