#pragma once
#include <stdint.h>

// SIM800L — 2G/GSM only.
//
// *** GPRS/HTTP IS NOT PURSUED ON THIS HARDWARE — see modem.cpp's file
// header. *** attach()/postJson()/closeIdle() below are intentionally left
// as stubs, not half-finished: this tracker's SIM800L is 2G-only, and the
// Philippines' NTC-mandated 2G/3G shutdown (complete 2026-12-31) has
// already been confirmed killing GPRS attach on the identical modem
// hardware on the gate scanner (PLAN.md §1b). Routine position now goes
// over SMS instead — see report.h. sleep()/wake() remain because SOS still
// benefits from the modem staying attached-and-sleeping rather than fully
// powered down, independent of whether GPRS itself ever comes up.
//
// TLS was going to run on the ESP32 (SSLClient over TinyGSM's raw socket),
// NOT the modem — do not use AT+HTTPSSL if this is ever revisited, SIM800
// SSL is firmware-dependent and not trustworthy for a child's location
// data. Moot for now: there's no GPRS socket for it to wrap.

namespace modem {
    bool begin();
    bool attach();              // stub — always false, see file header
    bool attached();            // stub — always false

    void sleep();               // AT+CSCLK=1, release DTR
    void wake();                // assert DTR, wait for AT response

    // Stub — always -1, see file header. No GPRS attach means nothing to
    // POST to; kept in the interface rather than deleted in case a future
    // 3G/4G-capable modem swap ever makes this viable again.
    int  postJson(const char* path, const char* json);
    void closeIdle();           // no-op without attach()

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
