#include "clock.h"
#include "../include/config.h"
#include <RTClib.h>
#include <WiFi.h>
#include <time.h>

static RTC_DS3231 s_rtc;
static bool     s_ok       = false;
static uint32_t s_lastSync = 0;

namespace clockw {

bool begin() {
    if (!s_rtc.begin()) { s_ok = false; return false; }
    // lostPower() means the backup cell is dead or was never fitted. Whatever
    // the RTC reports after that is fiction, so treat it as no clock at all.
    s_ok = !s_rtc.lostPower();
    return s_ok;
}

bool ok() { return s_ok; }

uint32_t now() { return s_ok ? s_rtc.now().unixtime() : 0; }

void syncFromNtp() {
    if (WiFi.status() != WL_CONNECTED) return;
    configTime(TZ_OFFSET_S, 0, NTP_SERVER);
    struct tm t;
    if (!getLocalTime(&t, 5000)) return;
    time_t epoch = mktime(&t);
    if (epoch < 1700000000) return;              // obviously wrong; ignore it
    s_rtc.adjust(DateTime((uint32_t)epoch));
    s_ok = true;                                 // a good NTP sync clears lostPower
    s_lastSync = (uint32_t)epoch;
}

uint32_t lastSync() { return s_lastSync; }

}
