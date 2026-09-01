#include "roster.h"
#include "clock.h"
#include "../include/config.h"
#include <Preferences.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <mbedtls/sha256.h>
#include <ArduinoJson.h>

static Preferences s_nvs;
static uint32_t s_hashes[ROSTER_MAX_CARDS];
static size_t   s_n = 0;
static uint32_t s_at = 0;

// 32-bit truncation of sha256(salt || uid). Enough to make accidental
// collisions negligible at a few hundred cards, and small enough that the whole
// roster is ~1.6 KB of NVS.
static uint32_t hashUid(const char* uid) {
    uint8_t out[32];
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, (const uint8_t*)ROSTER_SALT, strlen(ROSTER_SALT));
    mbedtls_sha256_update(&c, (const uint8_t*)uid, strlen(uid));
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
    return ((uint32_t)out[0] << 24) | ((uint32_t)out[1] << 16) |
           ((uint32_t)out[2] << 8)  |  (uint32_t)out[3];
}

namespace roster {

bool begin() {
    if (!s_nvs.begin("roster", false)) return false;
    s_at = s_nvs.getUInt("at", 0);
    size_t bytes = s_nvs.getBytesLength("h");
    if (bytes && bytes <= sizeof s_hashes) {
        s_nvs.getBytes("h", s_hashes, bytes);
        s_n = bytes / sizeof(uint32_t);
    }
    return true;
}

bool known(const char* uid) {
    if (!s_n) return false;
    uint32_t h = hashUid(uid);
    for (size_t i = 0; i < s_n; i++) if (s_hashes[i] == h) return true;
    return false;
}

bool stale() { return !s_at || (clockw::now() - s_at) > ROSTER_TTL_S; }

size_t   size()      { return s_n; }
uint32_t fetchedAt() { return s_at; }

bool refresh() {
    WiFiClient      plain;
    WiFiClientSecure tls;
    if (API_USE_TLS) tls.setInsecure();   // TODO: pin the CA before production
    WiFiClient& client = API_USE_TLS ? (WiFiClient&)tls : (WiFiClient&)plain;

    HTTPClient http;
    if (!http.begin(client, String(API_BASE) + "/functions/v1/roster")) return false;
    http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);
    int code = http.GET();
    if (code != 200) { http.end(); return false; }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) return false;

    // The server sends hex hashes, already salted — the raw UIDs never travel.
    size_t n = 0;
    for (JsonVariant v : doc["h"].as<JsonArray>()) {
        if (n >= ROSTER_MAX_CARDS) break;
        s_hashes[n++] = (uint32_t)strtoul(v.as<const char*>(), nullptr, 16);
    }
    s_n = n;
    s_at = clockw::now();
    s_nvs.putBytes("h", s_hashes, s_n * sizeof(uint32_t));
    s_nvs.putUInt("at", s_at);
    return true;
}

}
