#include "relay.h"
#include "smsq.h"
#include "modem.h"
#include "../include/config.h"
#include <Arduino.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static uint32_t s_lastPoll = 0;

// Hand-rolled scan for the KNOWN {"messages":[{"id":N,"to":"..","body":".."},..]}
// shape — not a general JSON parser (no escape handling), matching the
// no-ArduinoJson constraint everywhere else in this build. Fine because we
// control both ends of this wire format.
static const char* findField(const char* s, const char* key) {
    const char* p = strstr(s, key);
    return p ? p + strlen(key) : nullptr;
}

static bool extractQuoted(const char* p, char* out, size_t outCap) {
    if (!p || *p != '"') return false;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n < outCap - 1) out[n++] = *p++;
    out[n] = 0;
    return true;
}

namespace relay {

void begin() {}

void service() {
    if (!RELAY_ENABLED || !modem::attached()) return;

    // Acks first: a claim the server thinks is still outstanding blocks the
    // message from being retried until the lease expires.
    char ref[SMS_REF_MAX]; bool sent;
    while (smsq::takeResult(ref, sizeof ref, &sent)) {
        char path[40];
        snprintf(path, sizeof path, "/functions/v1/outbox/%s/ack", ref);
        char body[24];
        snprintf(body, sizeof body, "{\"sent\":%s}", sent ? "true" : "false");
        char resp[48];
        modem::request("POST", path, DEVICE_TOKEN, body, resp, sizeof resp);
    }

    uint32_t now = millis();
    if (now - s_lastPoll < RELAY_POLL_MS) return;
    s_lastPoll = now;

    if (smsq::depth() >= SMS_QUEUE_DEPTH || !smsq::ready()) return;

    // Not static, same reasoning as net.cpp's drain() buffers — this only
    // needs to exist while service() runs, not permanently.
    // limit=1, not the ESP32's limit=2: one full Job's worth of RAM (SMS_BODY_MAX
    // etc.) is already tight; claiming a second message here just to let it
    // sit in a full smsq is wasted lease time on the server's side.
    char resp[200];   // {"messages":[{...}]} for limit=1
    int code = modem::request("GET", "/functions/v1/outbox?limit=1", DEVICE_TOKEN, nullptr, resp, sizeof resp);
    if (code != 200) return;

    const char* idP   = findField(resp, "\"id\":");
    const char* toP   = findField(resp, "\"to\":");
    const char* bodyP = findField(resp, "\"body\":");
    if (!idP || !toP || !bodyP) return;   // no messages, or a shape we didn't expect

    char id[SMS_REF_MAX];
    snprintf(id, sizeof id, "%ld", strtol(idP, nullptr, 10));

    char to[SMS_NUMBER_MAX];
    char text[SMS_BODY_MAX];
    if (!extractQuoted(toP, to, sizeof to)) return;
    if (!extractQuoted(bodyP, text, sizeof text)) return;

    // The parent's number lives in RAM for this send only, same guarantee
    // as the ESP32 build — nothing about a relayed message is written to
    // EEPROM.
    smsq::enqueue(to, text, id);
}

}
