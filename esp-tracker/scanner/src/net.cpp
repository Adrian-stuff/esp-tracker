#include "net.h"
#include "store.h"
#include "clock.h"
#include "../include/config.h"
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

// Custom parameters for the config portal
static WiFiManagerParameter s_paramApi("api", "API Base URL", API_BASE, 64);
static WiFiManagerParameter s_paramToken("token", "Device Token", DEVICE_TOKEN, 64);

// Called when credentials are saved via the portal
static void saveCallback() {
    Serial.println(F("[net] Portal: saved, rebooting..."));
    delay(500);
    ESP.restart();
}

namespace net {

void begin() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.setHostname("tracker-scanner");

    // Set config portal callback
    s_wm.setSaveConfigCallback(saveCallback);

    // Add custom parameters to the portal
    s_wm.addParameter(&s_paramApi);
    s_wm.addParameter(&s_paramToken);

    // Portal settings
    s_wm.setConfigPortalTimeout(WIFI_AP_TIMEOUT_S);
    s_wm.setMinimumSignalQuality(15);

    // Try to connect with saved credentials.
    // autoConnect() starts the AP if no saved creds or connection fails.
    // The AP stays up on 192.168.4.1 even after STA connects.
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
    if (API_USE_TLS) tls.setInsecure();
    WiFiClient& client = API_USE_TLS ? (WiFiClient&)tls : (WiFiClient&)plain;

    HTTPClient http;
    if (!http.begin(client, String(API_BASE) + "/api/ingest/taps")) return 0;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);
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
    if (API_USE_TLS) tls.setInsecure();
    WiFiClient& client = API_USE_TLS ? (WiFiClient&)tls : (WiFiClient&)plain;

    HTTPClient http;
    if (!http.begin(client, String(API_BASE) + "/api/relay/sms")) return false;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);
    int code = http.POST(body);
    http.end();
    return code == 200;
}

}
