#include "net.h"
#include "store.h"
#include "clock.h"
#include "../include/config.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static uint32_t s_lastTry  = 0;
static uint32_t s_backoff  = 2000;

namespace net {

void begin() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    // NOTE: school networks are often WPA2-Enterprise or behind a captive
    // portal, neither of which an ESP32 handles gracefully. Settle the VLAN
    // with the IT contact before building the enclosure — see README.
}

bool online() { return WiFi.status() == WL_CONNECTED; }

void service() {
    if (online()) { s_backoff = 2000; return; }
    uint32_t now = millis();
    if (now - s_lastTry < s_backoff) return;
    s_lastTry = now;
    s_backoff = s_backoff < 60000 ? s_backoff * 2 : 60000;
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
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
        // The scanner texts the parent itself on the SIM900. Telling the server
        // means it skips its own message, so one tap is never two texts.
        t["device_sms_sent"] = batch[i].sms_sent;
        // Direction is NOT sent. The server infers it from the card's own
        // history that day, which is the only place that survives this device
        // rebooting, and the only place that can reorder taps that were
        // buffered offline and arrived late.
    }
    String body;
    serializeJson(doc, body);

    // Plain HTTP against a laptop on the LAN during local dev; TLS in
    // production. Never ship API_USE_TLS = false to anything off your bench —
    // these are a child's movements in clear text.
    WiFiClient      plain;
    WiFiClientSecure tls;
    if (API_USE_TLS) tls.setInsecure();   // TODO: pin the CA before production
    Client& client = API_USE_TLS ? (Client&)tls : (Client&)plain;

    HTTPClient http;
    if (!http.begin(client, String(API_BASE) + "/functions/v1/ingest")) return 0;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);
    int code = http.POST(body);
    http.end();

    if (code != 200) return 0;      // stays queued; the next pass retries
    store::commit(n);               // 200 IS the ack
    return n;
}

}
