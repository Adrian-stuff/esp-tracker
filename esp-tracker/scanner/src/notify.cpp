#include "notify.h"
#include "smsq.h"
#include "clock.h"
#include "settings.h"
#include "../include/config.h"
#include <Arduino.h>
#include <string.h>

// Small ring of "who did we just text about", so a child fiddling with their
// card does not turn into ten messages.
struct Recent { char uid[24]; uint32_t at; };
static Recent s_recent[12];
static uint8_t s_ri = 0;

static bool onCooldown(const char* uid, uint32_t now) {
    for (auto& r : s_recent)
        if (r.at && strcmp(r.uid, uid) == 0 && now - r.at < SMS_PER_CARD_COOLDOWN_S)
            return true;
    return false;
}

static void remember(const char* uid, uint32_t now) {
    Recent& r = s_recent[s_ri++ % 12];
    strncpy(r.uid, uid, sizeof r.uid - 1);
    r.uid[sizeof r.uid - 1] = 0;
    r.at = now;
}

namespace notify {

void begin() { memset(s_recent, 0, sizeof s_recent); }

bool onTap(const Tap& t, const char* childName, const char* direction) {
    if (!SMS_DIRECT_MODE) return false;      // gateway mode handles it elsewhere
    if (!smsq::ready()) return false;        // no network: the HTTPS path is the record

    uint32_t now = clockw::now();
    if (!now) return false;                  // no trustworthy clock, no timestamp to quote
    if (onCooldown(t.uid, now)) return false;

    // Local time, because "07:52" is the only form a parent can act on.
    uint32_t local = now + (uint32_t)(TZ_OFFSET_S);
    int hh = (local % 86400) / 3600, mm = (local % 3600) / 60;

    char body[152];
    snprintf(body, sizeof body, "%s tapped %s at %02d:%02d.",
             childName && *childName ? childName : "Your child", direction, hh, mm);

    bool queued = smsq::enqueue(settings::smsPrimary(), body);
    if (settings::smsSecondary()[0]) smsq::enqueue(settings::smsSecondary(), body);
    if (queued) remember(t.uid, now);
    return queued;
}

}
