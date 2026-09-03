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

    // Register a callback that fires before the immediate SMS sends —
    // used by main.cpp to disable BLE (~8mA) and by sos.cpp itself to
    // disable WiFi (~100mA) and GPS (~45mA), reducing peak current draw
    // to avoid LiPo BMS brownout during the SIM800L's 2A TX burst.
    typedef void (*PowerDownFn)();
    void onPowerDown(PowerDownFn fn);

    bool active();

    // True when sos.cpp is NOT in the middle of its own immediate 2-step
    // send (parent, then scanner) for the current trigger — i.e. idle
    // (nothing queued yet) or done trying (success or failure either way).
    // main.cpp uses this to hold off calling store::drain() while a send is
    // in flight: both this module and store::drain() can send the same
    // queued SOS to the scanner, and without this guard drain() can win the
    // race and send it before sos.cpp's own "try immediately" attempt ever
    // gets a chance to run, defeating the point of trying immediately first.
    bool smsIdle();

    // Fires on the 2s hold. Starts parallel acquisition, then transmits at
    // SOS_TX_DEADLINE_MS with whatever fix exists — see config.h.
    void trigger();

    // Second long-hold inside SOS_CANCEL_WINDOW_MS aborts an accidental press.
    void cancel();

    // Fires on real end-to-end confirmation for the event id — not merely
    // "the request was sent" — and is the only thing that plays Cue::Acked
    // ("your parent has been told"). Called from main.cpp's
    // modem::pollSmsCommand onAck callback, for an inbound
    // "<SMS_CMD_SECRET> ACK <id>" SMS relayed via the scanner's outbox
    // relay after relay-sms confirms this SOS reached the server — see
    // store.h's file header for the full round-trip.
    void onServerAck(const char* event_id);
}
