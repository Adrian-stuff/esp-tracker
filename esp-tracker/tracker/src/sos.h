#pragma once
#include <stdint.h>

// The safety-critical path. PLAN.md 2.
//
// Two haptic patterns, and the second one matters more than it looks:
//   PRESS  -> short buzz          "it registered"
//   ACKED  -> double buzz         "your parent has been told"
// The second is the detail most easily cut and least worth cutting.

namespace sos {
    void begin();
    void service();

    bool active();

    // Fires on the 2s hold. Starts parallel acquisition, then transmits at
    // SOS_TX_DEADLINE_MS with whatever fix exists — see config.h.
    void trigger();

    // Second long-hold inside SOS_CANCEL_WINDOW_MS aborts an accidental press.
    void cancel();

    // Called on HTTP 200 for the event id — not when the request is sent.
    void onServerAck(const char* event_id);
}
