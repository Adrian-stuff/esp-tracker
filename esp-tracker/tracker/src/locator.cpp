#include "locator.h"
#include "gps.h"
#include "modem.h"
#include "../include/config.h"
#include <WiFi.h>

namespace locator {

void begin() {
    // Station mode without connecting — scanning only.
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
}

void beginAcquire() {
    gps::power(true);
    WiFi.scanNetworks(true /* async */, false);
    // TODO: kick off BLE scan for known anchors, and AT+CENG for cell id
}

bool knownPlace(Fix& out) { (void)out; return false; }  // TODO: match BSSID set against registered places

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
        return true;
    }

    // TODO: Wi-Fi BSSID list -> server-side resolution (API key stays on the
    // server, never on the device). Then cell id as the floor.
    //
    // Send SSID alongside BSSID and RSSI, capped at ~6 APs. The SSID is the
    // only part a parent can recognise in the dashboard when naming a place,
    // and ~20 bytes each is nothing against a 4-6 MB/month budget.
    return false;
}

void service() { gps::service(); }

}
