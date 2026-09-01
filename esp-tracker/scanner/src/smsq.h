#pragma once
#include <stdint.h>
#include <stddef.h>
#include <HardwareSerial.h>

// SIM900 outbound SMS, as a queue with a non-blocking state machine.
//
// WHY A QUEUE. AT+CMGS is a multi-step exchange: send the command, wait for the
// '>' prompt, send the body, send Ctrl-Z, then wait several seconds for OK. On
// 2G that is 3-6 s. Doing it inline from the tap handler would stall the reader
// for the whole exchange, and a gate is a queue of thirty children — the same
// reason the feedback cues had to stop using delay().
//
// So a tap ENQUEUES and returns immediately; service() advances one step per
// loop() and the reader keeps accepting cards throughout.
//
// TIME-SENSITIVITY. Unlike the tap records, these are NOT retried forever. A
// text saying "Ana tapped in" that arrives three hours late is worse than
// nothing — it reads as a fresh event. If a message cannot be sent within
// SMS_MAX_ATTEMPTS it is dropped, and the tap still reaches the dashboard by
// the normal HTTPS path, which is the durable record.

namespace smsq {
    bool begin();
    void service();               // call every loop()

    // false when the queue is full. Never blocks.
    // `ref` is opaque to smsq and comes back through takeResult(), which is how
    // the relay knows WHICH server message it just delivered.
    bool enqueue(const char* number, const char* text, const char* ref = nullptr);

    // Pops one finished send. Returns false when there is nothing to report.
    bool takeResult(char* ref, size_t refLen, bool* sent);

    bool   ready();               // modem registered on the network
    size_t depth();
    int8_t signalQuality();       // AT+CSQ, -1 unknown

    // For SMS inbox polling (sms.cpp): access to the UART2 serial port.
    // Only call when isIdle() returns true to avoid conflicts with the
    // outbound state machine.
    HardwareSerial& serial();
    bool isIdle();                // modem is in Idle state, safe for other AT cmds
}
