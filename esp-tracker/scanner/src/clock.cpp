#include "clock.h"
#include "../include/config.h"
#include "../include/pins.h"
#include <RtcDS1302.h>
#include <WiFi.h>
#include <time.h>

static ThreeWire s_wire(PIN_RTC_DAT, PIN_RTC_CLK, PIN_RTC_CE);
static RtcDS1302<ThreeWire> s_rtc(s_wire);
static bool     s_ok       = false;
static uint32_t s_lastSync = 0;

namespace clockw {

bool begin() {
    s_rtc.Begin();

    // The DS1302 has a clock-halt (CH) bit in the seconds register.
    // If the battery died, the clock stopped and CH is set.
    // After a successful NTP sync we clear CH so the clock runs.
    s_rtc.SetIsRunning(true);

    // Check if the datetime is plausible (not all zeros or 0x00 fields).
    s_ok = s_rtc.IsDateTimeValid();
    return s_ok;
}

bool ok() { return s_ok; }

uint32_t now() {
    if (!s_ok) return 0;
    RtcDateTime dt = s_rtc.GetDateTime();
    return dt.Unix32Time();
}

void syncFromNtp() {
    if (WiFi.status() != WL_CONNECTED) return;
    configTime(TZ_OFFSET_S, 0, NTP_SERVER);
    struct tm t;
    if (!getLocalTime(&t, 5000)) return;
    time_t epoch = mktime(&t);
    if (epoch < 1700000000) return;              // obviously wrong; ignore it

    RtcDateTime dt;
    dt.InitWithUnix32Time((uint32_t)epoch);
    s_rtc.SetDateTime(dt);
    s_rtc.SetIsRunning(true);                    // clear CH bit so clock runs
    s_ok = true;
    s_lastSync = (uint32_t)epoch;
}

uint32_t lastSync() { return s_lastSync; }

}
