#pragma once
#include <stdint.h>

// WHICH tracker events are worth a text, and how often.
//
// Kept separate from modem.cpp because these are editorial decisions, not
// electrical ones: who hears about what, and how much is too much.
//
// The rule that shapes the list: an SMS costs the parent's attention, and
// attention spent on routine noise is not available for the SOS. Position
// reports are therefore NEVER texted — at the moving cadence that would be
// roughly 700 messages a day and would bury the one that matters.

enum class Event : uint8_t {
    Sos,            // always, immediately, no cooldown — see sos.cpp
    GeofenceExit,   // "left school" — the one a parent actually watches for
    GeofenceEnter,
    BatteryLow,     // 20%
    BatteryCritical // 10%
};

namespace notify {
    void begin();
    void service();

    // Applies per-event cooldowns and edge-detection, then hands the message to
    // the modem. Returns true if a message was queued, which the caller reports
    // to the server as device_sms_sent so it does not send a second one.
    bool fire(Event e, const char* detail);
}
