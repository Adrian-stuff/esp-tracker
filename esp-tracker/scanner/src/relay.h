#pragma once

// Sending SMS on the SERVER's behalf.
//
// Both modems are paid for already, and on a matching PH network a device-sent
// text is effectively free where a provider charges per message. So the server
// offers routine notifications to the devices first and only pays if nobody
// delivers in time.
//
// This scanner may relay ANY message — it is fixed, shared infrastructure, and
// the parent numbers it handles are never written to flash. A tracker is scoped
// by the server to its own child, whose parent number it already holds.
//
// SOS never arrives here. An emergency must not wait for a poll, a claim and a
// confirmation, so the escalation ladder goes straight to the provider.

namespace relay {
    void begin();
    void service();     // polls for work and acks completions; never blocks
}
