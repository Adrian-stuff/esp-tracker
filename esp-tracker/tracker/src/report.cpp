#include "report.h"
#include "locator.h"
#include "motion.h"
#include "modem.h"
#include "../include/config.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include "mbedtls/sha256.h"

static uint32_t s_lastSent = 0;

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

// First 4 bytes of sha256(SMS_CMD_SECRET + payload), hex (8 chars + NUL) —
// MUST match server/app/tracker_sms.py's _code() byte for byte, including
// exactly which characters are in `payload` (no "LOC " prefix, no trailing
// comma — see service() below for where that string is built). Same
// truncated-SHA256-over-a-shared-secret shape already used elsewhere in
// this project (scanner-uno/src/roster.cpp's hashUid(), server/app/
// attendance.py's roster_hash()) — proportionate to what this channel
// needs, not real end-to-end crypto (see SMS_CMD_SECRET's own comment).
//
// Using the *_ret mbedTLS entry points, not the plain mbedtls_sha256_*
// ones — this ESP32 core's sha256.h marks those MBEDTLS_DEPRECATED in
// favour of the _ret variants (confirmed against the actual installed
// framework-arduinoespressif32 package, not assumed from a mismatched
// mbedTLS version's docs).
static void hashCode(const char* payload, char out[9]) {
    unsigned char digest[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts_ret(&ctx, 0);   // 0 = SHA-256, not SHA-224
    mbedtls_sha256_update_ret(&ctx, (const unsigned char*)SMS_CMD_SECRET, strlen(SMS_CMD_SECRET));
    mbedtls_sha256_update_ret(&ctx, (const unsigned char*)payload, strlen(payload));
    mbedtls_sha256_finish_ret(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    for (uint8_t i = 0; i < 4; i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
}

namespace report {

void begin() { s_lastSent = 0; }

void service() {
    uint32_t interval = (motion::state() == MotionState::Moving)
        ? REPORT_INTERVAL_MOVING_MS : REPORT_INTERVAL_STATIONARY_MS;
    uint32_t now = millis();
    if (s_lastSent && now - s_lastSent < interval) return;

    Fix fix;
    if (!locator::best(fix)) return;   // nothing resolved yet — try again next service() call

    // Exactly the substring server/app/tracker_sms.py's parse() reconstructs
    // from its regex capture groups and re-hashes — %.6f (~11cm at the
    // equator) keeps this short while giving both sides an unambiguous,
    // already-formatted string to agree on, rather than each side
    // re-serializing a float and risking a platform rounding mismatch.
    char payload[64];
    snprintf(payload, sizeof payload, "%c,%.6f,%.6f,%d,%lu",
             sourceCode(fix.source), fix.lat, fix.lon,
             (int)fix.accuracy_m, (unsigned long)fix.recorded_at);

    char code[9];
    hashCode(payload, code);

    char msg[96];
    snprintf(msg, sizeof msg, "LOC %s,%s", payload, code);

    // Best-effort, same as every other routine (non-SOS) send in this
    // project (see smsq.h) — a failed send just retries at the next
    // service() call on the same cadence rather than being queued/retried
    // immediately, which is the right tradeoff for a channel this
    // low-stakes and this frequent.
    if (modem::sendSms(SCANNER_SMS_NUMBER, msg)) s_lastSent = now;
}

}
