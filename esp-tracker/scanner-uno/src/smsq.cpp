#include "smsq.h"
#include "modem.h"
#include "../include/config.h"
#include <string.h>

struct Job {
    char number[SMS_NUMBER_MAX];
    char text[SMS_BODY_MAX];
    char ref[SMS_REF_MAX];
    bool used;
};
static Job s_jobs[SMS_QUEUE_DEPTH];

struct Result { char ref[SMS_REF_MAX]; bool sent; bool used; };
static Result s_results[SMS_QUEUE_DEPTH];

namespace smsq {

bool begin() {
    memset(s_jobs, 0, sizeof s_jobs);
    memset(s_results, 0, sizeof s_results);
    return true;
}

size_t depth() {
    size_t n = 0;
    for (auto& j : s_jobs) if (j.used) n++;
    return n;
}

bool ready() { return modem::attached(); }

bool enqueue(const char* number, const char* text, const char* ref) {
    for (auto& j : s_jobs) {
        if (j.used) continue;
        strncpy(j.number, number, sizeof j.number - 1); j.number[sizeof j.number - 1] = 0;
        strncpy(j.text, text, sizeof j.text - 1);       j.text[sizeof j.text - 1] = 0;
        if (ref) { strncpy(j.ref, ref, sizeof j.ref - 1); j.ref[sizeof j.ref - 1] = 0; }
        else j.ref[0] = 0;
        j.used = true;
        return true;
    }
    return false;   // full
}

static void pushResult(const char* ref, bool sent) {
    if (!ref || !ref[0]) return;   // direct-mode sends (no server ref) have nothing to ack
    for (auto& r : s_results) {
        if (r.used) continue;
        strncpy(r.ref, ref, sizeof r.ref - 1); r.ref[sizeof r.ref - 1] = 0;
        r.sent = sent;
        r.used = true;
        return;
    }
    // Results queue full: drop silently. relay.cpp's next ack cycle will
    // simply retry the send later via the server's own lease expiry.
}

void service() {
    for (auto& j : s_jobs) {
        if (!j.used) continue;
        bool ok = modem::sendSms(j.number, j.text);
        pushResult(j.ref, ok);
        j.used = false;
        return;   // one send per call — keeps a single loop() iteration bounded
    }
}

bool takeResult(char* ref, size_t refLen, bool* sent) {
    for (auto& r : s_results) {
        if (!r.used) continue;
        strncpy(ref, r.ref, refLen - 1); ref[refLen - 1] = 0;
        *sent = r.sent;
        r.used = false;
        return true;
    }
    return false;
}

}
