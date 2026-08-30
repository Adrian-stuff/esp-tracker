#pragma once

// Outbox relay — same job as the ESP32 build's relay.cpp: poll the
// server's queued outbound messages, hand them to smsq, ack the ones that
// were sent or failed. Runs on its own timer (RELAY_POLL_MS), completely
// decoupled from tap timing — see config.h's "gateway mode" note for why
// that decoupling is the answer to "don't fetch the parent's number on the
// tap's hot path."

namespace relay {
    void begin();
    void service();
}
