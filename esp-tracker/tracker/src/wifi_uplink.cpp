#include "wifi_uplink.h"
#include "../include/config.h"
#include "locator.h"
#include "battery.h"
#include "modem.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <sys/time.h>

static Preferences s_prefs;
static char s_ssid[33] = "";
static char s_pass[65] = "";

static bool     s_connecting   = false;
static uint32_t s_connectStart = 0;
static uint32_t s_lastConnectAttempt = 0;
static constexpr uint32_t CONNECT_TIMEOUT_MS      = 10000;
static constexpr uint32_t RECONNECT_BACKOFF_MS    = 30000;

static uint32_t s_lastLocPush    = 0;
static uint32_t s_lastOutboxPoll = 0;
static uint32_t s_lastMockPoll   = 0;
static uint32_t s_locSeq = 0;

// Bounded HTTP timeouts everywhere below — same lesson as modem.cpp's
// sendSms(): an unbounded blocking call on the main loop starves
// serviceButton() (SOS detection) for however long it takes. WiFi/HTTPS to
// a real server is usually fast, but "usually" is not a bound.
static constexpr uint32_t HTTP_TIMEOUT_MS = 6000;

static const char* sourceString(FixSource s) {
    switch (s) {
        case FixSource::Gnss:      return "gnss";
        case FixSource::Wifi:      return "wifi";
        case FixSource::BleAnchor: return "ble_anchor";
        case FixSource::Cell:      return "cell";
        case FixSource::Mock:      return "mock";
        default:                   return "cell";
    }
}

namespace wifi_uplink {

void begin() {
    s_prefs.begin("tracker", true);  // read-only, same namespace as SOS/scanner numbers
    String ssid = s_prefs.getString("wifi_ssid", WIFI_UPLINK_SSID);
    String pass = s_prefs.getString("wifi_pass", WIFI_UPLINK_PASS);
    s_prefs.end();
    strncpy(s_ssid, ssid.c_str(), sizeof(s_ssid) - 1); s_ssid[sizeof(s_ssid) - 1] = '\0';
    strncpy(s_pass, pass.c_str(), sizeof(s_pass) - 1); s_pass[sizeof(s_pass) - 1] = '\0';

    if (!WIFI_UPLINK_ENABLED || !s_ssid[0] || strcmp(s_ssid, "change-me") == 0) {
        Serial.println("[wifi_uplink] disabled or no SSID configured — skipping (SMS reporting is unaffected)");
        return;
    }
    // WIFI_STA is already the mode locator.cpp sets (scan-only, never
    // associating, by its own original design). This module is what
    // actually makes it associate now — see this file's own header comment
    // on the resulting interaction with motion.cpp's periodic scans.
    WiFi.mode(WIFI_STA);
    Serial.printf("[wifi_uplink] configured for SSID \"%s\" — will connect in background\n", s_ssid);
}

bool connected() { return WiFi.status() == WL_CONNECTED; }

static void serviceConnection() {
    if (!WIFI_UPLINK_ENABLED || !s_ssid[0] || strcmp(s_ssid, "change-me") == 0) return;

    if (WiFi.status() == WL_CONNECTED) {
        if (s_connecting) {
            Serial.printf("[wifi_uplink] connected — IP %s\n", WiFi.localIP().toString().c_str());
            s_connecting = false;
        }
        return;
    }

    uint32_t now = millis();
    if (s_connecting) {
        if (now - s_connectStart >= CONNECT_TIMEOUT_MS) {
            Serial.println("[wifi_uplink] connect attempt timed out — will retry after backoff");
            WiFi.disconnect();
            s_connecting = false;
            s_lastConnectAttempt = now;
        }
        return;   // still trying, non-blocking — WiFi.begin() itself is async
    }

    if (s_lastConnectAttempt && now - s_lastConnectAttempt < RECONNECT_BACKOFF_MS) return;

    Serial.printf("[wifi_uplink] connecting to \"%s\"...\n", s_ssid);
    WiFi.begin(s_ssid, s_pass);
    s_connecting   = true;
    s_connectStart = now;
}

// Direct-to-Supabase location push — bypasses the scanner entirely.
static void pushLocation() {
    Fix fix;
    if (!locator::best(fix)) return;   // nothing to send yet, try again next tick

    WiFiClientSecure tls;
    tls.setInsecure();   // same pragmatic choice already made throughout
                         // this codebase's WiFi+HTTPS calls (scanner/src/*)
    HTTPClient http;
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(tls, String(SUPABASE_URL) + "/functions/v1/ingest")) return;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);

    struct timeval tv; gettimeofday(&tv, nullptr);
    char id[40];
    snprintf(id, sizeof id, "%s-wifi-%lu-%lu", DEVICE_ID, (unsigned long)tv.tv_sec, (unsigned long)++s_locSeq);

    JsonDocument doc;
    JsonArray events = doc["events"].to<JsonArray>();
    JsonObject ev = events.add<JsonObject>();
    ev["id"]          = id;
    ev["lat"]         = fix.lat;
    ev["lon"]         = fix.lon;
    ev["accuracy_m"]  = fix.accuracy_m;
    ev["source"]      = sourceString(fix.source);
    ev["recorded_at"] = fix.recorded_at ? fix.recorded_at : (uint32_t)tv.tv_sec;
    ev["battery_pct"] = battery::pct();
    String body;
    serializeJson(doc, body);

    Serial.printf("[wifi_uplink] pushing location direct to Supabase: %s\n", body.c_str());
    int code = http.POST(body);
    http.end();
    Serial.printf("[wifi_uplink] ingest POST -> HTTP %d\n", code);
}

// Poll the server for a mock location — demo/presentation mode. When the
// tracker is indoors and GPS can't fix, dev-mock inserts fake coordinates
// into Supabase. This function fetches the latest one so report.cpp can
// send it via SMS instead of "no fix". One small GET, negligible cost.
static void pollMockLocation() {
    WiFiClientSecure tls;
    tls.setInsecure();
    HTTPClient http;
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(tls, String(SUPABASE_URL) + "/functions/v1/mock-location")) return;
    http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);
    int code = http.GET();
    if (code != 200) {
        http.end();
        return;
    }
    String resp = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, resp)) return;
    if (!doc["ok"].as<bool>()) return;

    double lat = doc["lat"].as<double>();
    double lon = doc["lon"].as<double>();
    float acc  = doc["accuracy_m"].as<float>();
    if (acc <= 0) acc = 12.0f;

    // recorded_at from server — parse ISO string to epoch if present
    uint32_t recordedAt = 0;
    const char* ts = doc["recorded_at"] | "";
    if (ts[0]) {
        struct tm tm = {};
        // Parse "2026-09-01T12:34:56.789Z" — strptime handles the Z
        if (strptime(ts, "%Y-%m-%dT%H:%M:%S", &tm)) {
            recordedAt = (uint32_t)mktime(&tm);
        }
    }
    if (!recordedAt) {
        struct timeval tv; gettimeofday(&tv, nullptr);
        recordedAt = (uint32_t)tv.tv_sec;
    }

    locator::setMock(lat, lon, acc, recordedAt);
}
// see this file's header comment. Mirrors scanner/src/relay.cpp's
// claim-then-ack pattern exactly (same server contract), just delivering
// via this device's own SIM800L instead of the scanner's SIM900A.
static void pollOutbox() {
    WiFiClientSecure tls;
    tls.setInsecure();
    HTTPClient http;
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(tls, String(SUPABASE_URL) + "/functions/v1/outbox?limit=2")) return;
    http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[wifi_uplink] outbox poll -> HTTP %d\n", code);
        http.end();
        return;
    }
    String resp = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, resp)) return;
    JsonArray messages = doc["messages"].as<JsonArray>();
    for (JsonObject m : messages) {
        long id          = m["id"].as<long>();
        const char* to   = m["to"]   | "";
        const char* body = m["body"] | "";
        if (!*to || !*body) continue;

        Serial.printf("[wifi_uplink] relaying outbox #%ld to %s (scanner SIM900A backup path): %s\n",
                      id, to, body);
        // Routine timeout (see config.h's ROUTINE_REG_TIMEOUT_MS comment):
        // this is a background relay, not time-critical — bounding it the
        // same way store::drain()/notify.cpp already do keeps this from
        // starving serviceButton() the same way an unbounded modem wait
        // would.
        bool sent = modem::sendSms(to, body, SMS_SEND_TIMEOUT_MS, ROUTINE_REG_TIMEOUT_MS);

        WiFiClientSecure ackTls;
        ackTls.setInsecure();
        HTTPClient ackHttp;
        ackHttp.setConnectTimeout(HTTP_TIMEOUT_MS);
        ackHttp.setTimeout(HTTP_TIMEOUT_MS);
        char ackUrl[160];
        snprintf(ackUrl, sizeof ackUrl, "%s/functions/v1/outbox/%ld/ack", SUPABASE_URL, id);
        if (ackHttp.begin(ackTls, ackUrl)) {
            ackHttp.addHeader("Content-Type", "application/json");
            ackHttp.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);
            char ackBody[32];
            snprintf(ackBody, sizeof ackBody, "{\"sent\":%s}", sent ? "true" : "false");
            ackHttp.POST(ackBody);
            ackHttp.end();
        }
        Serial.printf("[wifi_uplink] outbox #%ld %s\n", id, sent ? "delivered" : "delivery FAILED (left pending for the scanner or a later retry)");
    }
}

void off() {
    // Unconditionally kill the WiFi radio. motion::begin() puts WiFi into
    // WIFI_STA for scanning regardless of whether the uplink is enabled, so
    // we can't gate on s_connecting or WiFi.status() — WiFi may be drawing
    // ~50mA in STA idle even when the uplink was never configured.
    if (WiFi.getMode() != WIFI_OFF) {
        Serial.println("[wifi_uplink] powering off WiFi radio (pre-SMS brownout mitigation)");
        WiFi.disconnect();
        WiFi.mode(WIFI_OFF);
        s_connecting = false;
    }
}

void restore() {
    // Re-enable WiFi STA after SOS SMS completes — motion.cpp needs it for
    // tierWifi() scanning. Does NOT auto-connect; serviceConnection() handles
    // that on the next loop iteration at its own backoff pace.
    if (WiFi.getMode() == WIFI_OFF) {
        Serial.println("[wifi_uplink] restoring WiFi STA mode (post-SOS)");
        WiFi.mode(WIFI_STA);
    }
}

void service() {
    serviceConnection();
    if (!connected()) return;

    uint32_t now = millis();
    if (now - s_lastLocPush >= WIFI_UPLINK_LOC_INTERVAL_MS) {
        s_lastLocPush = now;
        pushLocation();
    }
    if (now - s_lastOutboxPoll >= WIFI_UPLINK_OUTBOX_POLL_MS) {
        s_lastOutboxPoll = now;
        pollOutbox();
    }
    if (now - s_lastMockPoll >= WIFI_UPLINK_MOCK_POLL_MS) {
        s_lastMockPoll = now;
        pollMockLocation();
    }
}

}
