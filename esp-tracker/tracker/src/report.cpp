#include "report.h"
#include "locator.h"
#include "motion.h"
#include "modem.h"
#include "../include/config.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "mbedtls/sha256.h"

static uint32_t s_lastSent = 0;
static uint32_t s_lastWifiSent = 0;

// Max APs to include in a WiFi scan SMS (160 char limit).
// 4 APs with SSID ≈ 148 chars. 5 APs ≈ 185 — too long.
static constexpr uint8_t WIFI_SCAN_MAX_APS = 4;
// Send WIFISCAN on the same cadence as motion scans (2 min moving, 5 min
// stationary) rather than a fixed 10 min.  Indoor position resolves only
// when the server sees the BSSIDs, so latency here IS the fix latency.
// A WIFISCAN SMS is ~140 bytes; at the moving cadence that's ~100 texts/day,
// well within prepaid budgets.
// The actual gating is done inside service(): we only send when the motion
// module has a fresh scan (s_lastWifiSent < s_lastScan equivalent).

// Hash of the last WiFi scan we actually sent, to skip duplicate sends.
// Uses the same FNV-1a as motion.cpp for consistency.
static uint32_t s_lastScanHash = 0;

static uint32_t hashScanPayload(const char* payload) {
    uint32_t h = 2166136261u;
    for (const char* p = payload; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    return h;
}

// Single-letter wire codes for Fix::source (locator.h) — MUST match
// server/app/tracker_sms.py's SOURCE_CODES exactly, or every report this
// sends fails to parse server-side.
static char sourceCode(FixSource s) {
    switch (s) {
        case FixSource::Gnss:      return 'g';
        case FixSource::Wifi:      return 'w';
        case FixSource::BleAnchor: return 'b';
        case FixSource::Cell:      return 'c';
        default:                   return 'c';
    }
}

static void hashCode(const char* payload, char out[9]) {
    unsigned char digest[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts_ret(&ctx, 0);
    mbedtls_sha256_update_ret(&ctx, (const unsigned char*)SMS_CMD_SECRET, strlen(SMS_CMD_SECRET));
    mbedtls_sha256_update_ret(&ctx, (const unsigned char*)payload, strlen(payload));
    mbedtls_sha256_finish_ret(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    for (uint8_t i = 0; i < 4; i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
}

// Send a WIFISCAN SMS with the latest motion scan data.
// Returns true if a message was actually sent (new data, not a duplicate).
static bool sendWifiScan(uint32_t now) {
    uint8_t n = motion::lastScanCount();
    if (n == 0) return false;

    const motion::ScanAp* aps = motion::lastScan();
    uint8_t toSend = n < WIFI_SCAN_MAX_APS ? n : WIFI_SCAN_MAX_APS;

    // Use system epoch timestamp — the server needs this for place-matching.
    // syncClockFromNetwork() sets the ESP32's clock from NITZ on boot.
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    uint32_t epoch = (uint32_t)tv.tv_sec;

    // Build payload: <epoch>,<bssid>:<rssi>:<ssid>,...
    char wpayload[128];
    int off = snprintf(wpayload, sizeof wpayload, "%lu", (unsigned long)epoch);
    for (uint8_t i = 0; i < toSend && off < (int)sizeof(wpayload) - 30; i++) {
        char bssid[18];
        snprintf(bssid, sizeof bssid, "%02X:%02X:%02X:%02X:%02X:%02X",
                 aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
                 aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5]);
        off += snprintf(wpayload + off, sizeof wpayload - off,
                        ",%s:%d:%s", bssid, aps[i].rssi, aps[i].ssid);
    }

    // Dedup: skip if the payload hash hasn't changed since last send.
    // Same BSSIDs at same signal = same place, no point sending again.
    uint32_t h = hashScanPayload(wpayload);
    if (h == s_lastScanHash) return false;

    char wcode[9];
    hashCode(wpayload, wcode);

    char wmsg[160];
    snprintf(wmsg, sizeof wmsg, "WIFISCAN %s,%s", wpayload, wcode);
    if (modem::sendSms(s_scannerNumber, wmsg)) {
        s_lastScanHash = h;
        s_lastWifiSent = now;
        return true;
    }
    return false;
}

namespace report {

void begin() { s_lastSent = 0; s_lastWifiSent = 0; s_lastScanHash = 0; }

void service() {
    uint32_t now = millis();

    // ---- LOC report: GPS-only, on motion-driven cadence ----
    uint32_t interval = (motion::state() == MotionState::Moving)
        ? REPORT_INTERVAL_MOVING_MS : REPORT_INTERVAL_STATIONARY_MS;
    if (s_lastSent && now - s_lastSent < interval) {
        // LOC cadence not met yet.  But WIFISCAN runs independently —
        // check that below even when LOC is throttled.
    } else {
        Fix fix;
        if (locator::best(fix)) {
            char payload[64];
            snprintf(payload, sizeof payload, "%c,%.6f,%.6f,%d,%lu",
                     sourceCode(fix.source), fix.lat, fix.lon,
                     (int)fix.accuracy_m, (unsigned long)fix.recorded_at);

            char code[9];
            hashCode(payload, code);

            char msg[96];
            snprintf(msg, sizeof msg, "LOC %s,%s", payload, code);

            if (modem::sendSms(s_scannerNumber, msg)) s_lastSent = now;
        }
    }

    // ---- WIFISCAN report: independent of GPS, sent on every motion scan ----
    // This is the indoor position path.  GPS is irrelevant indoors — the server
    // resolves BSSIDs against known places.  Send whenever the motion module
    // has fresh scan data we haven't sent yet.
    sendWifiScan(now);
}

}
