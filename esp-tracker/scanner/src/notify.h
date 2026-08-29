#pragma once
#include "reader.h"
#include <stdint.h>

// Decides WHICH taps are worth a text, and phrases them.
//
// Kept separate from smsq (which only knows how to talk to a SIM900) because
// the interesting decisions are editorial, not electrical: who hears about
// what, how often, and in what words. Those change far more than the AT
// commands do.

namespace notify {
    void begin();

    // Called for every accepted tap. Applies the per-card cooldown and the
    // direct/gateway mode, then enqueues. Returns true if a text was queued —
    // which the caller reports to the server as device_sms_sent, so the server
    // does not send a second one for the same tap.
    bool onTap(const Tap& t, const char* childName, const char* direction);
}
