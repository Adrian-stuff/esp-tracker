#include "gps.h"
#include "../include/pins.h"
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>

static TinyGPSPlus  s_gps;
static HardwareSerial s_serial(1);
static uint32_t s_lastFixMs = 0;

namespace gps {

void begin() {
    pinMode(PIN_GPS_EN, OUTPUT);
    power(false);
    s_serial.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
}

void power(bool on) { digitalWrite(PIN_GPS_EN, on ? HIGH : LOW); }

void service() {
    while (s_serial.available()) s_gps.encode(s_serial.read());
    // Track when we last had a valid fix
    if (s_gps.location.isValid()) s_lastFixMs = millis();
}

bool fix(GnssFix& out) {
    if (!s_gps.location.isValid()) return false;
    out.valid      = true;
    out.lat        = s_gps.location.lat();
    out.lon        = s_gps.location.lng();
    out.satellites = s_gps.satellites.value();
    out.speed_mps  = s_gps.speed.mps();
    out.heading    = s_gps.course.deg();
    // HDOP is not accuracy, but it is the only proxy the NEO-6M gives us.
    out.accuracy_m = s_gps.hdop.isValid() ? s_gps.hdop.hdop() * 5.0f : 50.0f;
    out.recorded_at = millis() / 1000;  // approximate, will be set by caller if needed
    return true;
}

bool hasFixSince(uint32_t since_ms) {
    if (!s_gps.location.isValid()) return false;
    // Check if the fix was received after since_ms
    return s_lastFixMs >= since_ms;
}

GnssDiag diagnostics() {
    GnssDiag d{};
    d.charsProcessed  = s_gps.charsProcessed();
    d.passedChecksum  = s_gps.passedChecksum();
    d.failedChecksum  = s_gps.failedChecksum();
    d.satellitesValid = s_gps.satellites.isValid();
    d.satellites      = d.satellitesValid ? (uint8_t)s_gps.satellites.value() : 0;
    d.hdopValid       = s_gps.hdop.isValid();
    d.hdop            = d.hdopValid ? (float)s_gps.hdop.hdop() : 0.0f;
    d.hasFix          = s_gps.location.isValid();
    return d;
}

}
