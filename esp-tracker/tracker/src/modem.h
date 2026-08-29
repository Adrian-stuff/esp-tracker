#pragma once
#include <stdint.h>

// SIM800L — 2G/GSM only.
//
// TLS runs on the ESP32 (SSLClient over TinyGSM's raw socket), NOT on the
// modem: do not use AT+HTTPSSL, SIM800 SSL is firmware-dependent and not
// trustworthy for a child's location data.
//
// The modem stays ATTACHED and in AT+CSCLK=1 sleep between bursts rather than
// powered down: a cold GSM attach costs 5-15s, and for a safety device latency
// beats runtime. GPRS itself comes up only when there is something to send.

namespace modem {
    bool begin();
    bool attach();              // GPRS up
    bool attached();

    void sleep();               // AT+CSCLK=1, release DTR
    void wake();                // assert DTR, wait for AT response

    // Returns the HTTP status. 200 IS the server-level ack — release the event
    // from the queue on 200 and nothing else. Reuse the connection across a
    // burst so the 3-8s handshake is paid once, not per event.
    int  postJson(const char* path, const char* json);
    void closeIdle();           // tear down GPRS after GPRS_IDLE_CLOSE_MS quiet

    // TLS validation needs a roughly-correct clock and the ESP32 has no RTC.
    bool syncClockFromNetwork();  // AT+CCLK? after NITZ

    // Direct SMS. Needs no GPRS, no TLS, and no server — it survives network
    // congestion, a misconfigured APN, and your server being down. This is the
    // reason the SIM800L substitution is not purely a downgrade.
    bool sendSms(const char* number, const char* text);

    // Cell tower identity — the position source of last resort.
    bool cellInfo(uint16_t& mcc, uint16_t& mnc, uint16_t& lac, uint32_t& cellId, int8_t& rssi);

    int8_t signalQuality();     // AT+CSQ, -1 on failure
}
