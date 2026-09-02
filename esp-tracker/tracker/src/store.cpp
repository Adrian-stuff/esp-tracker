#include "store.h"
#include "modem.h"
#include "../include/config.h"
#include <Preferences.h>
#include <cstdio>
#include <cstring>
#include "mbedtls/sha256.h"

// NVS-backed store-and-forward queue. NVS is simpler than LittleFS and
// survives OTA. Capacity is limited by NVS blob size (typically 4000 bytes
// for a namespace). With QueuedEvent at ~550 bytes each, we can fit ~7 entries.
//
// SOS events ALWAYS succeed — they evict the oldest Telemetry entry if needed.
// Telemetry events are the lowest priority and get evicted first.

static Preferences s_nvs;
static const char* NS     = "store";
static const char* KEY_Q  = "queue";     // blob: array of QueuedEvent
static const char* KEY_N  = "count";     // uint32: number of entries
static const char* KEY_H  = "head";      // uint32: index of oldest entry
static const char* KEY_SQ = "seq";       // uint32: monotonic sequence for ID uniqueness

static constexpr uint8_t CAPACITY = 7;    // ~4KB NVS blob limit

static QueuedEvent s_buf[CAPACITY];
static uint8_t     s_count = 0;
static uint8_t     s_head  = 0;
static uint32_t    s_seq   = 0;

static void save() {
    // Write head, count, and sequence
    s_nvs.putUInt(KEY_H, s_head);
    s_nvs.putUInt(KEY_N, s_count);
    s_nvs.putUInt(KEY_SQ, s_seq);
    // Write entries in logical order (oldest first)
    QueuedEvent ordered[CAPACITY];
    for (uint8_t i = 0; i < s_count; i++)
        ordered[i] = s_buf[(s_head + i) % CAPACITY];
    s_nvs.putBytes(KEY_Q, ordered, s_count * sizeof(QueuedEvent));
}

static uint8_t physIdx(uint8_t logical) { return (s_head + logical) % CAPACITY; }

static void generateId(char* out, size_t len) {
    s_seq++;
    snprintf(out, len, "%s-%lu-%lu", DEVICE_ID,
             (unsigned long)(millis() / 1000), (unsigned long)s_seq);
}

namespace store {

bool begin() {
    if (!s_nvs.begin(NS, false)) return false;
    s_head  = s_nvs.getUInt(KEY_H, 0);
    s_count = s_nvs.getUInt(KEY_N, 0);
    s_seq   = s_nvs.getUInt(KEY_SQ, 0);
    if (s_count > CAPACITY) { s_count = 0; s_head = 0; }
    if (s_count > 0) {
        QueuedEvent ordered[CAPACITY];
        size_t bytesRead = s_nvs.getBytes(KEY_Q, ordered, sizeof(ordered));
        if (bytesRead != s_count * sizeof(QueuedEvent)) { s_count = 0; s_head = 0; return true; }
        // Re-map into ring buffer
        for (uint8_t i = 0; i < s_count; i++)
            s_buf[(s_head + i) % CAPACITY] = ordered[i];
    }
    return true;
}

bool push(QueuedEvent& ev) {
    // SOS always succeeds: evict oldest Telemetry if full
    if (s_count >= CAPACITY) {
        if (ev.kind == EventKind::Sos) {
            // Find and evict the oldest Telemetry entry
            bool evicted = false;
            for (uint8_t i = 0; i < s_count; i++) {
                uint8_t idx = physIdx(i);
                if (s_buf[idx].kind == EventKind::Telemetry) {
                    Serial.printf("[store] queue full — evicting telemetry entry %s to make room for SOS\n", s_buf[idx].id);
                    // Shift everything after this entry down
                    for (uint8_t j = i; j + 1 < s_count; j++)
                        s_buf[physIdx(j)] = s_buf[physIdx(j + 1)];
                    s_count--;
                    evicted = true;
                    break;
                }
            }
            // If still full after evicting all Telemetry, drop oldest anyway
            if (s_count >= CAPACITY) {
                Serial.printf("[store] queue still full after telemetry eviction — dropping oldest entry %s\n", s_buf[s_head].id);
                s_head = (s_head + 1) % CAPACITY;
                s_count--;
            } else if (!evicted) {
                Serial.println("[store] queue full, no telemetry to evict — dropping oldest entry regardless (SOS never fails to queue)");
            }
        } else {
            Serial.println("[store] queue full — non-SOS event dropped (not queued)");
            return false;   // non-SOS events fail when full
        }
    }

    ev.attempts = 0;
    generateId(ev.id, sizeof(ev.id));   // written into the caller's copy too — see store.h
    uint8_t idx = physIdx(s_count);
    s_buf[idx] = ev;
    s_count++;
    save();
    Serial.printf("[store] queued %s (%s), depth now %u/%u\n",
                  ev.id, ev.kind == EventKind::Sos ? "SOS" : "telemetry", s_count, CAPACITY);
    return true;
}

bool peek(QueuedEvent& out) {
    if (!s_count) return false;
    // SOS events jump the queue: scan for the first SOS, otherwise head
    for (uint8_t i = 0; i < s_count; i++) {
        uint8_t idx = physIdx(i);
        if (s_buf[idx].kind == EventKind::Sos) {
            out = s_buf[idx];
            return true;
        }
    }
    out = s_buf[s_head];
    return true;
}

void ack(const char* id) {
    if (!s_count) return;
    // Find the entry with matching id and remove it
    for (uint8_t i = 0; i < s_count; i++) {
        uint8_t idx = physIdx(i);
        if (strncmp(s_buf[idx].id, id, sizeof(s_buf[idx].id)) == 0) {
            // Shift everything after this entry down
            for (uint8_t j = i; j + 1 < s_count; j++)
                s_buf[physIdx(j)] = s_buf[physIdx(j + 1)];
            s_count--;
            save();
            Serial.printf("[store] acked+removed %s, depth now %u/%u\n", id, s_count, CAPACITY);
            return;
        }
    }
    Serial.printf("[store] ack(%s) — no matching entry (already removed, or stale/unknown id)\n", id);
}

size_t depth() { return s_count; }

uint32_t backoff_ms(uint8_t attempts) {
    // 2s, 4s, 8s ... capped at 5 minutes. Never gives up.
    uint32_t ms = 2000UL << (attempts > 8 ? 8 : attempts);
    return ms > 300000UL ? 300000UL : ms;
}

// Hash code matching report.cpp and server/app/tracker_sms.py.
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

// Single-letter wire codes matching report.cpp and server/app/tracker_sms.py.
static char sourceCode(const char* src) {
    if (strcmp(src, "gnss") == 0) return 'g';
    if (strcmp(src, "wifi") == 0) return 'w';
    if (strcmp(src, "ble_anchor") == 0) return 'b';
    return 'c';
}

static uint32_t s_lastDrainAttempt = 0;

void drain() {
    if (!s_count) return;

    uint32_t now = millis();
    // Only try once per backoff interval
    if (s_lastDrainAttempt && now - s_lastDrainAttempt < backoff_ms(0)) return;

    QueuedEvent ev;
    if (!peek(ev)) return;

    // Check backoff
    if (ev.attempts > 0) {
        uint32_t wait = backoff_ms(ev.attempts);
        if (now - s_lastDrainAttempt < wait) return;
    }

    s_lastDrainAttempt = now;

    bool sent = false;

    if (ev.kind == EventKind::Sos) {
        // SOS retry: send location SMS to the SCANNER (see the retry-target
        // note further down) — not the parent.
        // Parse payload JSON to extract lat/lon/accuracy
        float lat = 0, lon = 0, acc = 0;
        char src[16] = "unknown";
        // Simple JSON parse — extract fields we need
        const char* p;
        if ((p = strstr(ev.payload, "\"lat\":"))) lat = atof(p + 6);
        if ((p = strstr(ev.payload, "\"lon\":"))) lon = atof(p + 6);
        if ((p = strstr(ev.payload, "\"accuracy_m\":"))) acc = atof(p + 12);
        if ((p = strstr(ev.payload, "\"source\":"))) {
            const char* s = p + 9;
            if (*s == '"') s++;
            uint8_t i = 0;
            while (*s && *s != '"' && i < sizeof(src) - 1) src[i++] = *s++;
            src[i] = '\0';
        }

        // Retries go to the SCANNER, not the parent. This queue exists to
        // guarantee the event reaches the SERVER (see store.h's file
        // header) — the parent already got a best-effort direct text from
        // sos.cpp's immediate send, and re-sending THAT copy here too would
        // double-text the parent for one button press (the bug this
        // comment used to sit next to). If the parent's copy never arrived,
        // the escalation ladder (dispatch cron, once this reaches Supabase)
        // is what retries reaching them — not this local queue.
        // " ID:<id>" lets relay-sms dedupe this retry against the same
        // button press (so it doesn't start a second escalation ladder)
        // and echo back a real ack — see sos.cpp's matching comment on the
        // immediate send for the full explanation. ev.id is this entry's
        // own queue id, generated once by store::push() and unchanged
        // across every retry attempt of it.
        char sms[160];
        if (lat != 0 || lon != 0) {
            snprintf(sms, sizeof sms,
                     "SOS from %s. https://maps.google.com/?q=%.5f,%.5f (+/-%dm) ID:%s",
                     DEVICE_ID, lat, lon, (int)acc, ev.id);
        } else {
            snprintf(sms, sizeof sms, "SOS from %s. Position unknown. ID:%s", DEVICE_ID, ev.id);
        }
        Serial.printf("[store] drain: retrying SOS %s (attempt %u)\n", ev.id, ev.attempts + 1);
        // Bounded registration wait — see ROUTINE_REG_TIMEOUT_MS's comment:
        // this runs from the main loop and would otherwise block
        // serviceButton() (a NEW child SOS hold) for far longer than
        // acceptable on a cold radio.
        sent = modem::sendSms(s_scannerNumber, sms, SMS_SEND_TIMEOUT_MS, ROUTINE_REG_TIMEOUT_MS);
    } else if (ev.kind == EventKind::Telemetry && ev.payload_len > 0) {
        // Telemetry: payload is already "LOC <src>,<lat>,<lon>,<acc>,<epoch>,<code>"
        // Just send it directly
        Serial.printf("[store] drain: retrying telemetry %s (attempt %u)\n", ev.id, ev.attempts + 1);
        sent = modem::sendSms(s_scannerNumber, ev.payload, SMS_SEND_TIMEOUT_MS, ROUTINE_REG_TIMEOUT_MS);
    }

    if (sent && ev.kind != EventKind::Sos) {
        // Telemetry: "sent" means the LOCAL MODEM accepted the SMS — not
        // confirmed server-side storage. Weaker than the queue's header
        // comment describes, but an accepted tradeoff for routine reports
        // that age out anyway (see store.h) — unlike SOS, handled below.
        ack(ev.id);
    } else if (sent && ev.kind == EventKind::Sos) {
        // SOS: deliberately NOT acked here even though the local modem
        // accepted it. Only a REAL ack from the server (sos::onServerAck(),
        // via modem::pollSmsCommand's onAck) removes an SOS entry — see
        // store.h's file header for the full round-trip. This entry stays
        // live and WILL be retried again on the next backoff interval if no
        // ack arrives in time; that's intentional, not a missed cleanup,
        // and safe because relay-sms dedupes retries by this same id.
        Serial.printf("[store] SOS %s delivered to modem — held pending real server ack (next retry in %lums if none arrives)\n",
                      ev.id, (unsigned long)backoff_ms(ev.attempts + 1));
    } else {
        // Local send failed outright — increment attempts, retry after backoff.
        for (uint8_t i = 0; i < s_count; i++) {
            uint8_t idx = physIdx(i);
            if (strncmp(s_buf[idx].id, ev.id, sizeof(s_buf[idx].id)) == 0) {
                s_buf[idx].attempts++;
                save();
                Serial.printf("[store] %s send failed, attempt %u — next retry in %lums\n",
                              ev.id, s_buf[idx].attempts, (unsigned long)backoff_ms(s_buf[idx].attempts));
                break;
            }
        }
    }
}

}
