#include "smsq.h"
#include "modem.h"
#include "../include/config.h"
#include <string.h>

struct Job {
    char number[SMS_NUMBER_MAX];
    char text[SMS_BODY_MAX];
    bool used;
};
static Job s_jobs[SMS_QUEUE_DEPTH];

namespace smsq {

bool begin() {
    memset(s_jobs, 0, sizeof s_jobs);
    return true;
}

size_t depth() {
    size_t n = 0;
    for (auto& j : s_jobs) if (j.used) n++;
    return n;
}

bool enqueue(const char* number, const char* text) {
    for (auto& j : s_jobs) {
        if (j.used) continue;
        strncpy(j.number, number, sizeof j.number - 1); j.number[sizeof j.number - 1] = 0;
        strncpy(j.text, text, sizeof j.text - 1);       j.text[sizeof j.text - 1] = 0;
        j.used = true;
        return true;
    }
    return false;   // full
}

void service() {
    for (auto& j : s_jobs) {
        if (!j.used) continue;
        modem::sendSms(j.number, j.text);
        j.used = false;
        return;   // one send per call — keeps a single loop() iteration bounded
    }
}

}
