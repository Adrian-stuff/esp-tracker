#include "report.h"
#include "locator.h"
#include "motion.h"
#include "modem.h"
#include "battery.h"
#include "gps.h"
#include "store.h"
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

    // Queued (store.cpp NVS) rather than sent directly — LiPo BMS brownout
    // mitigation, see config.h's MODEM_CFUN_IDLE_ENABLED block. Persisting
    // BEFORE the radio ever keys up means a brownout mid-send can't lose
    // this: it survives the reboot and store::drain() retries it.
    QueuedEvent ev{};
    ev.kind = EventKind::Telemetry;
    ev.recorded_at = epoch;
    ev.payload_len = (uint16_t)snprintf(ev.payload, sizeof ev.payload, "%s", wmsg);
    Serial.printf("[report] WIFISCAN due (%u APs, new since last send) — queuing for next radio window\n", toSend);
    if (store::push(ev)) {
        s_lastScanHash = h;
        s_lastWifiSent = now;
        return true;
    }
    Serial.println("[report] WIFISCAN queue push failed (queue full?) — will retry next scan cycle");
    return false;
}

// GPS acquisition state for routine LOC reports — GAP FIXED: before this,
// nothing outside sos::trigger() ever called gps::power(true), so
// locator::best() could NEVER return a GNSS fix during routine operation
// (knownPlace()/Wi-Fi/cell resolution are still TODO stubs in locator.cpp —
// see its own comments). Routine LOC reports were consequently dead code in
// practice: report::service() checked locator::best() every cycle, but it
// could only ever succeed during the few seconds after an actual SOS. This
// state machine gives routine reporting its own GPS duty cycle, independent
// of SOS, so a tracker that is never pressed still reports real GPS
// positions when it has sky visibility.
static bool     s_acquiring     = false;
static uint32_t s_acquireStart  = 0;

namespace report {

void begin() { s_lastSent = 0; s_lastWifiSent = 0; s_lastScanHash = 0; s_acquiring = false; }

void service() {
    uint32_t now = millis();

    // ---- LOC report: GPS-only, on motion-driven cadence ----
    uint32_t interval = (motion::state() == MotionState::Moving)
        ? REPORT_INTERVAL_MOVING_MS : REPORT_INTERVAL_STATIONARY_MS;

    if (!s_acquiring && (!s_lastSent || now - s_lastSent >= interval)) {
        // Due, and not already trying: power the GPS on and start the
        // clock on this attempt. gps.h: V_BCKP stays powered regardless of
        // this gating, so ephemeris survives and a warm start is ~1s, not
        // a 27s cold one — this is not as expensive as it looks.
        Serial.printf("[report] LOC due (%s) — powering on GPS to acquire a fix\n",
                      motion::state() == MotionState::Moving ? "moving" : "stationary");
        locator::beginAcquire();
        s_acquiring    = true;
        s_acquireStart = now;
    }

    if (s_acquiring) {
        Fix fix;
        if (locator::best(fix)) {
            // Battery rides on the LOC report rather than a separate message:
            // it changes slowly, LOC already goes out on a state-driven
            // cadence, and a dedicated battery SMS would be one more message
            // competing with the ~700/day this already costs while moving.
            char payload[80];
            snprintf(payload, sizeof payload, "%c,%.6f,%.6f,%d,%lu,%u",
                     sourceCode(fix.source), fix.lat, fix.lon,
                     (int)fix.accuracy_m, (unsigned long)fix.recorded_at,
                     (unsigned)battery::pct());

            char code[9];
            hashCode(payload, code);

            char msg[112];
            snprintf(msg, sizeof msg, "LOC %s,%s", payload, code);

            Serial.printf("[report] fix acquired after %lums, battery %u%% — queuing for next radio window\n",
                          (unsigned long)(now - s_acquireStart), (unsigned)battery::pct());

            // Queued (store.cpp NVS) rather than sent directly — same LiPo
            // BMS brownout mitigation as WIFISCAN above: persisted before
            // the radio ever keys up, so a brownout mid-send can't lose it.
            QueuedEvent ev{};
            ev.kind = EventKind::Telemetry;
            ev.recorded_at = fix.recorded_at;
            ev.payload_len = (uint16_t)snprintf(ev.payload, sizeof ev.payload, "%s", msg);
            if (store::push(ev)) {
                s_lastSent = now;
            } else {
                Serial.println("[report] LOC queue push failed (queue full?) — will retry next cadence tick");
            }
            gps::power(false);   // back to power-gated until the next cycle
            s_acquiring = false;
        } else if (now - s_acquireStart >= FIX_BUDGET_GNSS_MS) {
            // Gave it a fair budget (matches config.h's FIX_BUDGET_GNSS_MS,
            // previously defined but unused anywhere) — indoors this WILL
            // happen every time, and that is expected, not a bug (same
            // framing as SOS_TX_DEADLINE_MS). Give up until the next cycle
            // rather than burning power holding GPS on forever.
            Serial.printf("[report] LOC: no fix within %lums — giving up until next cycle\n",
                          (unsigned long)FIX_BUDGET_GNSS_MS);
            gps::power(false);
            s_acquiring = false;
            s_lastSent  = now;   // consumes this interval's attempt either way
        }
        // else: still trying, say nothing more this call — gps::service()
        // (called every loop() iteration via sos::service()/locator::service()
        // whenever an SOS is ALSO active, or here below) keeps feeding NMEA.
    }

    // ---- WIFISCAN report: independent of GPS, sent on every motion scan ----
    // This is the indoor position path.  GPS is irrelevant indoors — the server
    // resolves BSSIDs against known places.  Send whenever the motion module
    // has fresh scan data we haven't sent yet.
    sendWifiScan(now);
}

}
