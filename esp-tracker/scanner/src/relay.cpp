#include "relay.h"
#include "smsq.h"
#include "net.h"
#include "settings.h"
#include "../include/config.h"
#include "../include/certs.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static uint32_t s_lastPoll = 0;

static bool request(const char* method, const String& url, const String& body, String& out) {
    WiFiClient plain; WiFiClientSecure tls;
    if (API_USE_TLS) tls.setCACert(AMAZON_ROOT_CA_1);
    WiFiClient& client = API_USE_TLS ? (WiFiClient&)tls : (WiFiClient&)plain;

    HTTPClient http;
    if (!http.begin(client, url)) return false;
    http.addHeader("Authorization", String("Bearer ") + settings::deviceToken());
    http.addHeader("Content-Type", "application/json");
    int code = strcmp(method, "POST") == 0 ? http.POST(body) : http.GET();
    if (code == 200) out = http.getString();
    http.end();
    return code == 200;
}

namespace relay {

void begin() {}

void service() {
    if (!RELAY_ENABLED || !net::online()) return;

    // Acks first: a claim the server thinks is still outstanding blocks the
    // message from being retried or paid for until the lease expires.
    char ref[16]; bool sent;
    while (smsq::takeResult(ref, sizeof ref, &sent)) {
        String body = String("{\"sent\":") + (sent ? "true" : "false") + "}";
        String ignored;
        request("POST", String(settings::apiBase()) + "/functions/v1/outbox/" + ref + "/ack", body, ignored);
    }

    uint32_t now = millis();
    if (now - s_lastPoll < RELAY_POLL_MS) return;
    s_lastPoll = now;

    // Only ask for what we can actually hold, or we would claim messages the
    // lease expires on while they sit in a full queue.
    if (smsq::depth() >= SMS_QUEUE_DEPTH - 1 || !smsq::ready()) return;

    String resp;
    if (!request("GET", String(settings::apiBase()) + "/functions/v1/outbox?limit=2", "", resp)) return;

    JsonDocument doc;
    if (deserializeJson(doc, resp)) return;
    for (JsonObject m : doc["messages"].as<JsonArray>()) {
        char id[16];
        snprintf(id, sizeof id, "%ld", (long)m["id"].as<long>());
        // The parent's number lives in RAM for this send only. Nothing about a
        // relayed message is written to flash.
        smsq::enqueue(m["to"], m["body"], id);
    }
}

}
