#include "locator.h"
#include "gps.h"
#include "modem.h"
#include "../include/config.h"
#include <WiFi.h>

static bool     s_mockValid = false;
static Fix      s_mockFix   = {};

namespace locator {

void begin() {
    // Station mode without connecting — scanning only.
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
}

void beginAcquire() {
    gps::power(true);
    // WiFi scanning is handled by motion::tierWifi() — do NOT start a
    // separate scan here; both fighting for the radio at the same time
    // corrupts results. motion.cpp runs every 2-5 min and stores the raw
    // BSSID/RSSI/SSID data that report.cpp sends as WIFISCAN SMS.
}

bool knownPlace(Fix& out) { (void)out; return false; }  // TODO: match BSSID set against registered places

void setMock(double lat, double lon, float accuracy_m, uint32_t recorded_at) {
    s_mockFix.source = FixSource::Mock;
    s_mockFix.lat = lat;
    s_mockFix.lon = lon;
    s_mockFix.accuracy_m = accuracy_m;
    s_mockFix.recorded_at = recorded_at;
    s_mockFix.place[0] = '\0';
    s_mockValid = true;
    Serial.printf("[locator] mock location set: %.6f, %.6f (acc %.0fm)\n", lat, lon, accuracy_m);
}

void clearMock() {
    if (s_mockValid) {
        Serial.println("[locator] mock location cleared (real fix available)");
        s_mockValid = false;
    }
}

bool hasMock() { return s_mockValid; }

bool best(Fix& out) {
    // Priority: a known place beats everything (it is exact and free), then
    // GNSS if it has actually resolved, then Wi-Fi, then cell.
    if (knownPlace(out)) return true;

    GnssFix g;
    if (gps::fix(g)) {
        out.source = FixSource::Gnss;
        out.lat = g.lat; out.lon = g.lon;
        out.accuracy_m = g.accuracy_m; out.recorded_at = g.recorded_at;
        out.place[0] = '\0';
        clearMock();   // real fix available — mock is obsolete
        return true;
    }

    // TODO: Wi-Fi BSSID list -> server-side resolution (API key stays on the
    // server, never on the device). Then cell id as the floor.

    // Mock location from server — used during demos when GPS can't fix
    // (indoors). Only after real sources have been checked, so a real GPS
    // fix always wins. The mock is set by wifi_uplink polling the
    // mock-location edge function and cleared when a real fix arrives.
    if (s_mockValid) {
        out = s_mockFix;
        return true;
    }

    return false;
}

void service() { gps::service(); }

}
