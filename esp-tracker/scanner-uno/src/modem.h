#pragma once
#include <stdint.h>
#include <stddef.h>

// SIM800L — the SHARED uplink (this build scrapped its original SIM900
// after extensive testing left it unresponsive on every baud rate, wiring
// orientation, and power configuration tried — see pins.h). Unlike the
// ESP32 scanner (Wi-Fi for the record, a separate SIM900 UART2 for SMS —
// two independent radios that
// never contend), the Uno has exactly one AT-command channel and everyone
// who wants the modem — the attendance drain, the roster refresh, the
// outbox relay, SMS sending — goes through this single arbiter. net.cpp,
// roster.cpp's refresh caller, relay.cpp, and smsq.cpp all call INTO this
// module rather than owning their own AT state machines.
//
// *** STILL FUNDAMENTALLY BLOCKING, BUT NO LONGER DEAF WHILE IT WAITS ***
// Each call below still blocks the CALLER (main.cpp's loop()) for the
// duration of one AT exchange — this is not the full per-loop() state
// machine the ESP32 build's smsq.cpp uses, and rewriting the AT layer
// itself into one is a much larger job than this build's single shared
// SoftwareSerial channel makes worth it right now. What changed: every
// blocking wait loop inside modem.cpp now calls an IdleHook (see
// setIdleHook() below) on each spin instead of doing nothing but polling
// the SIM800L — main.cpp registers a hook that services the buzzer and
// (throttled, so it doesn't starve SoftwareSerial's RX buffer) polls the
// RFID reader. A tap during a GPRS attach or SMS send is now still
// accepted and queued, not silently dropped for the whole blocking window
// — it was that silence, not the blocking itself, that made the scanner
// look "stuck."
//
// *** THE SIM800L CANNOT SPEAK TLS *** — see config.h. Everything here is
// plain HTTP over a raw TCP socket (AT+CIPSTART), not the modem's built-in
// AT+HTTP* command set, so the request/response format is fully under our
// control and RAM-predictable.

namespace modem {
    typedef void (*IdleHook)();
    // Called repeatedly from inside every blocking wait below — keep it
    // fast and throttle anything that touches SPI/other bit-banged buses
    // internally (see main.cpp's registered hook for why).
    void setIdleHook(IdleHook hook);

    bool begin();
    bool ensureAttached();                              // GPRS up; cheap no-op if already attached
    bool attached();                                     // last-known state, for a health indicator — does not itself talk to the modem
    bool syncClockFromNetwork(uint32_t& epochOut);       // AT+CCLK?, NITZ-disciplined

    // Buffered request/response — fine for small JSON (ingest ack, outbox
    // poll, outbox ack). Returns the HTTP status code, or -1 on failure.
    // respBuf is NUL-terminated on success, truncated if the reply is
    // larger than respCap.
    int request(const char* method, const char* path, const char* authBearer,
                const char* body, char* respBuf, size_t respCap);

    // Roster refresh streams straight into roster::refreshAdd() as bytes
    // arrive — the reply can hold more hashes than any buffer this MCU can
    // spare. Returns true only if the whole response was read to a clean
    // close with HTTP 200.
    bool requestRosterStream(const char* path, const char* authBearer);

    bool sendSms(const char* number, const char* text);
    int8_t signalQuality();                              // AT+CSQ, -1 unknown
}
