#pragma once
#include <stddef.h>

// SIM800L outbound SMS queue. Simpler than ../src/smsq.h: this build has no
// server to ack a sent/failed result back to (see modem.h), so there is no
// `ref` field and no takeResult() — just enqueue and service(). Same
// contract otherwise: enqueue() never blocks (reader::poll() never touches
// the modem at all); only service()'s own send blocks, bounded by
// SMS_SEND_TIMEOUT_MS.

namespace smsq {
    bool begin();
    void service();     // pops and sends ONE queued message per call

    bool   enqueue(const char* number, const char* text);
    size_t depth();
}
