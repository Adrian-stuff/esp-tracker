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

static const char* eventName(Event e) {
    switch (e) {
        case Event::Sos:             return "SOS";
        case Event::GeofenceExit:    return "GeofenceExit";
        case Event::GeofenceEnter:   return "GeofenceEnter";
        case Event::BatteryLow:      return "BatteryLow";
        case Event::BatteryCritical: return "BatteryCritical";
        default:                     return "Unknown";
    }
}

bool fire(Event e, const char* detail) {
    if (!NOTIFY_SMS_ENABLED && e != Event::Sos) {
        Serial.printf("[notify] %s suppressed — NOTIFY_SMS_ENABLED is false\n", eventName(e));
        return false;
    }

    uint32_t now = millis() / 1000;
    uint8_t  i   = (uint8_t)e;
    uint32_t cd  = cooldownFor(e);
    if (cd && s_last[i] && now - s_last[i] < cd) {
        Serial.printf("[notify] %s suppressed — %lus into a %lus cooldown\n",
                      eventName(e), (unsigned long)(now - s_last[i]), (unsigned long)cd);
        return false;
    }

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

    Serial.printf("[notify] firing %s -> %s: %s\n", eventName(e), s_sosNumber, body);
    // Bounded registration wait (see ROUTINE_REG_TIMEOUT_MS in config.h):
    // checkBattery() calls this straight from loop(), not gated on
    // sos::smsIdle() — an unbounded cold-radio wait here would block
    // serviceButton() (a NEW SOS hold starting right now) the same way an
    // unbounded store::drain() send would.
    if (!modem::sendSms(s_sosNumber, body, SMS_SEND_TIMEOUT_MS, ROUTINE_REG_TIMEOUT_MS)) {
        Serial.printf("[notify] %s send failed\n", eventName(e));
        return false;
    }
    s_last[i] = now;
    return true;
}

}
