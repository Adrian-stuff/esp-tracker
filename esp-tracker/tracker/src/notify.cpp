#include "notify.h"
#include "modem.h"
#include "../include/config.h"
#include <Arduino.h>

// Last time each event was sent, so a device sitting on a geofence boundary
// cannot text the parent every two minutes.
static uint32_t s_last[5] = {0};

static uint32_t cooldownFor(Event e) {
    switch (e) {
        case Event::Sos:             return 0;                  // never suppressed
        case Event::GeofenceExit:
        case Event::GeofenceEnter:   return NOTIFY_GEOFENCE_COOLDOWN_S;
        default:                     return NOTIFY_BATTERY_COOLDOWN_S;
    }
}

namespace notify {

void begin()   { for (auto& t : s_last) t = 0; }
void service() { }

bool fire(Event e, const char* detail) {
    if (!NOTIFY_SMS_ENABLED && e != Event::Sos) return false;

    uint32_t now = millis() / 1000;
    uint8_t  i   = (uint8_t)e;
    uint32_t cd  = cooldownFor(e);
    if (cd && s_last[i] && now - s_last[i] < cd) return false;

    char body[152];
    switch (e) {
        case Event::GeofenceExit:
            snprintf(body, sizeof body, "%s left %s.", CHILD_NAME, detail ? detail : "the area"); break;
        case Event::GeofenceEnter:
            snprintf(body, sizeof body, "%s arrived at %s.", CHILD_NAME, detail ? detail : "the area"); break;
        case Event::BatteryLow:
            snprintf(body, sizeof body, "%s's tracker is at 20%% battery.", CHILD_NAME); break;
        case Event::BatteryCritical:
            snprintf(body, sizeof body, "%s's tracker is at 10%% battery and will stop soon.", CHILD_NAME); break;
        default:
            snprintf(body, sizeof body, "%s: %s", CHILD_NAME, detail ? detail : ""); break;
    }

    if (!modem::sendSms(SOS_SMS_PRIMARY, body)) return false;
    s_last[i] = now;
    return true;
}

}
