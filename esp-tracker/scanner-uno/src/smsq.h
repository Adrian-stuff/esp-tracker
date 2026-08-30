#pragma once
#include <stdint.h>
#include <stddef.h>

// SIM800L outbound SMS queue — same enqueue/service/takeResult contract as
// the ESP32 build's smsq.h, so relay.cpp barely changes between the two
// targets. The one real difference: service() here calls modem::sendSms()
// synchronously (bounded by SMS_SEND_TIMEOUT_MS) rather than stepping
// through the AT exchange one phase per loop() — see modem.h's KNOWN
// LIMITATION note. Reads are still never blocked (reader::poll() doesn't
// touch the modem at all), only enqueued sends are.

namespace smsq {
    bool begin();
    void service();               // pops and sends ONE queued message per call

    bool enqueue(const char* number, const char* text, const char* ref = nullptr);
    bool takeResult(char* ref, size_t refLen, bool* sent);

    bool   ready();                // last-known GPRS/modem state — see net::online()
    size_t depth();
}
