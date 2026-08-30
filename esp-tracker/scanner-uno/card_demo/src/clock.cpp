#include "clock.h"
#include "../include/pins.h"
#include <ThreeWire.h>
#include <RtcDS1302.h>

static ThreeWire        s_wire(PIN_RTC_IO, PIN_RTC_SCLK, PIN_RTC_CE);
static RtcDS1302<ThreeWire> s_rtc(s_wire);
static bool     s_ok       = false;
static uint32_t s_lastSync = 0;

namespace clockw {

bool begin() {
    s_rtc.Begin();
    // IsDateTimeValid() here is only a register-range sanity check (see
    // clock.h's warning) — GetIsRunning() adds the clock-halt bit as a
    // second, equally imperfect signal. Together they catch a DS1302 that's
    // clearly uninitialized; neither catches a silent backup-power loss.
    s_ok = s_rtc.IsDateTimeValid() && s_rtc.GetIsRunning();
    return s_ok;
}

bool ok() { return s_ok; }

uint32_t now() { return s_ok ? s_rtc.GetDateTime().Unix32Time() : 0; }

void setFromEpoch(uint32_t epoch) {
    if (epoch < 1700000000UL) return;   // obviously wrong; ignore it
    RtcDateTime dt(0);
    dt.InitWithUnix32Time(epoch);
    s_rtc.SetIsWriteProtected(false);   // SetDateTime() silently no-ops otherwise
    s_rtc.SetDateTime(dt);
    s_rtc.SetIsRunning(true);
    s_ok = true;                        // a good sync clears any doubt
    s_lastSync = epoch;
}

uint32_t lastSync() { return s_lastSync; }

}
