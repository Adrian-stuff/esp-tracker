#pragma once
#include <stdint.h>

// SIM800L — SMS + clock only. This is the one deliberate difference from
// ../src/modem.h: that file also drives AT+CIPSTART/CIPSEND/+IPD framing
// for a plain-HTTP ingest/roster/outbox path over GPRS. This build has none
// of that — see ../README.md for why (short version: GPRS_ENABLED has been
// false on the real firmware since GPRS attach was confirmed failing on the
// bench signal, which made that whole code path dead weight at runtime,
// just not at compile time). AT+CMGS (SMS) and AT+CCLK (clock) both work
// over plain network registration, independent of a GPRS/data attach, so
// neither needs any of it.
//
// Still blocking, same as ../src/modem.h: each call below blocks the
// caller for one AT exchange. setIdleHook() buys back tap-responsiveness
// during that wait the same way — see main.cpp's registered hook.

namespace modem {
    typedef void (*IdleHook)();
    void setIdleHook(IdleHook hook);

    bool begin();                                   // AT ping, ATE0, text-mode SMS
    bool registered();                               // AT+CREG? — true if attached to the home/roaming network (needed for SMS; NOT the same as a GPRS attach — see README.md's GPRS section)
    bool syncClockFromNetwork(uint32_t& epochOut);   // AT+CCLK?, NITZ-disciplined

    bool sendSms(const char* number, const char* text);
    int8_t signalQuality();                          // AT+CSQ, -1 unknown
}
