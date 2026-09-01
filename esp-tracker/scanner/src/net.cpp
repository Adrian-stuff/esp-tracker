#include "net.h"
#include "store.h"
#include "clock.h"
#include "settings.h"
#include "../include/config.h"
#include "../include/certs.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>

static uint32_t s_lastTry  = 0;
static uint32_t s_backoff  = 2000;
static bool     s_portalActive = false;
static WiFiManager s_wm;

// Custom parameters for the config portal. Values are set from settings::*()
// in begin() — NOT here — because settings::begin() (which loads NVS) runs
// at setup() time, and these statics would otherwise be constructed before
// that, from whatever the compiler baked in as a placeholder.
static WiFiManagerParameter s_paramApi("api", "API Base URL", "", 64);
static WiFiManagerParameter s_paramToken("token", "Device Token", "", 64);
static WiFiManagerParameter s_paramSmsPrimary("sms1", "Parent SMS (primary)", "", 20);
static WiFiManagerParameter s_paramSmsSecondary("sms2", "Parent SMS (secondary, optional)", "", 20);

// Called when the portal's Save is pressed. WiFiManager persists the WiFi
// credentials itself; everything else on this page is ours to persist.
static void saveCallback() {
    settings::setApiBase(s_paramApi.getValue());
    settings::setDeviceToken(s_paramToken.getValue());
    settings::setSmsPrimary(s_paramSmsPrimary.getValue());
    settings::setSmsSecondary(s_paramSmsSecondary.getValue());
    Serial.println(F("[net] Portal: saved, rebooting..."));
    delay(500);
    ESP.restart();
}

namespace net {

void begin() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.setHostname("tracker-scanner");

    // Pre-fill the portal with whatever's currently active, not the
    // compiled-in defaults — so opening the portal to change one field
    // doesn't silently reset the others.
    s_paramApi.setValue(settings::apiBase(), 64);
    s_paramToken.setValue(settings::deviceToken(), 64);
    s_paramSmsPrimary.setValue(settings::smsPrimary(), 20);
    s_paramSmsSecondary.setValue(settings::smsSecondary(), 20);

    // Set config portal callback
    s_wm.setSaveConfigCallback(saveCallback);

    // Add custom parameters to the portal
    s_wm.addParameter(&s_paramApi);
    s_wm.addParameter(&s_paramToken);
    s_wm.addParameter(&s_paramSmsPrimary);
    s_wm.addParameter(&s_paramSmsSecondary);

    // Portal settings
    s_wm.setConfigPortalTimeout(WIFI_AP_TIMEOUT_S);
    s_wm.setMinimumSignalQuality(15);

    // Try default compile-time credentials first, before opening the portal.
    // This avoids forcing every unit through 192.168.4.1 when the SSID and
    // password are already known at compile time.
    if (strlen(WIFI_SSID) > 0) {
        Serial.printf("[net] Trying default SSID: %s\n", WIFI_SSID);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
            delay(250);
            Serial.print(".");
        }
        Serial.println();
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[net] Connected to %s, IP=%s\n",
                          WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
            WiFi.setAutoReconnect(true);
            return;
        }
        Serial.println(F("[net] Default credentials failed, starting portal..."));
        WiFi.disconnect();
    }

    // Fall back to config portal.
    Serial.println(F("[net] Starting WiFiManager..."));
    if (!s_wm.autoConnect(AP_SSID, AP_PASS)) {
        Serial.println(F("[net] Portal timed out, rebooting..."));
        delay(1000);
        ESP.restart();
    }

    // If we get here, either we connected to saved creds or the user saved new ones.
    s_portalActive = false;
    WiFi.setAutoReconnect(true);

    Serial.printf("[net] Connected to %s, IP=%s\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
}

bool online() { return WiFi.status() == WL_CONNECTED; }

bool portalActive() { return s_portalActive; }

void service() {
    if (online()) { s_backoff = 2000; return; }
    uint32_t now = millis();
    if (now - s_lastTry < s_backoff) return;
    s_lastTry = now;
    s_backoff = s_backoff < 60000 ? s_backoff * 2 : 60000;

    // If disconnected, try reconnecting with saved creds
    WiFi.disconnect();
    WiFi.begin();  // uses saved credentials from NVS
}

size_t drain() {
    if (!online() || !store::depth()) return 0;

    static Tap batch[BATCH_MAX];
    size_t n = store::peekBatch(batch, BATCH_MAX);
    if (!n) return 0;

    JsonDocument doc;
    JsonArray taps = doc["taps"].to<JsonArray>();
    for (size_t i = 0; i < n; i++) {
        JsonObject t = taps.add<JsonObject>();
        t["id"]          = batch[i].id;
        t["card_uid"]    = batch[i].uid;
        t["recorded_at"] = batch[i].recorded_at;
        t["device_sms_sent"] = batch[i].sms_sent;
    }
    String body;
    serializeJson(doc, body);

    WiFiClient      plain;
    WiFiClientSecure tls;
    if (API_USE_TLS) tls.setCACert(AMAZON_ROOT_CA_1);
    WiFiClient& client = API_USE_TLS ? (WiFiClient&)tls : (WiFiClient&)plain;

    // /functions/v1/ingest, not /api/ingest/taps: Supabase only serves the
    // former (see supabase/functions/ingest) — the FastAPI dev server aliases
    // it to the same handler, so this one path works against both backends.
    HTTPClient http;
    if (!http.begin(client, String(settings::apiBase()) + "/functions/v1/ingest")) return 0;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + settings::deviceToken());
    int code = http.POST(body);
    http.end();

    if (code != 200) return 0;
    store::commit(n);
    return n;
}

bool postRelaySms(const char* sender, const char* text) {
    if (!online()) return false;

    JsonDocument doc;
    doc["sender"] = sender;
    doc["text"]   = text;
    String body;
    serializeJson(doc, body);

    WiFiClient      plain;
    WiFiClientSecure tls;
    if (API_USE_TLS) tls.setCACert(AMAZON_ROOT_CA_1);
    WiFiClient& client = API_USE_TLS ? (WiFiClient&)tls : (WiFiClient&)plain;

    HTTPClient http;
    if (!http.begin(client, String(settings::apiBase()) + "/api/relay/sms")) return false;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + settings::deviceToken());
    int code = http.POST(body);
    http.end();
    return code == 200;
}

}
