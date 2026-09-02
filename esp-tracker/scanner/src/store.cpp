#include "store.h"
#include "../include/config.h"
#include <LittleFS.h>

// Append-only log plus a read cursor. Simpler and far more crash-tolerant than
// rewriting a file on every tap: a power cut mid-write costs one record, not
// the whole queue.
static const char* LOG_PATH    = "/taps.log";
static const char* CURSOR_PATH = "/taps.cur";

struct Rec { char uid[24]; uint32_t recorded_at; char id[24]; uint8_t sms_sent; };
struct DailyTap { char uid[24]; uint8_t count; };
static DailyTap s_daily[ROSTER_MAX_CARDS];
static size_t   s_dailyCount = 0;
static uint32_t s_currentDay = 0;

static uint32_t s_cursor = 0;      // records already delivered

static uint32_t readCursor() {
    File f = LittleFS.open(CURSOR_PATH, "r");
    if (!f) return 0;
    uint32_t v = 0; f.read((uint8_t*)&v, sizeof v); f.close();
    return v;
}
static void writeCursor(uint32_t v) {
    File f = LittleFS.open(CURSOR_PATH, "w");
    if (f) { f.write((const uint8_t*)&v, sizeof v); f.close(); }
}
static uint32_t records() {
    File f = LittleFS.open(LOG_PATH, "r");
    if (!f) return 0;
    uint32_t n = f.size() / sizeof(Rec); f.close();
    return n;
}

namespace store {

bool begin() {
    if (!LittleFS.begin(true)) return false;
    s_cursor = readCursor();

    // Compact once the delivered prefix dominates, so the log cannot grow
    // without bound across a term.
    if (s_cursor > QUEUE_COMPACT_AT) {
        File in = LittleFS.open(LOG_PATH, "r");
        File out = LittleFS.open("/taps.tmp", "w");
        if (in && out) {
            in.seek(s_cursor * sizeof(Rec));
            uint8_t buf[256];
            int n;
            while ((n = in.read(buf, sizeof buf)) > 0) out.write(buf, n);
            in.close(); out.close();
            LittleFS.remove(LOG_PATH);
            LittleFS.rename("/taps.tmp", LOG_PATH);
            s_cursor = 0;
            writeCursor(0);
        }
    }

    // Pre-populate today's tap counts from stored log (reboot recovery)
    File log = LittleFS.open(LOG_PATH, "r");
    if (log) {
        Rec r;
        while (log.read((uint8_t*)&r, sizeof r) == sizeof r) {
            recordTap(r.uid, r.recorded_at);
        }
        log.close();
    }
    return true;
}

uint8_t recordTap(const char* uid, uint32_t recorded_at) {
    if (!uid || !*uid || !recorded_at) return 1;
    uint32_t day = (recorded_at + (uint32_t)TZ_OFFSET_S) / 86400;
    if (day != s_currentDay) {
        s_currentDay = day;
        s_dailyCount = 0;
    }
    for (size_t i = 0; i < s_dailyCount; i++) {
        if (strcmp(s_daily[i].uid, uid) == 0) {
            s_daily[i].count++;
            return s_daily[i].count;
        }
    }
    if (s_dailyCount < ROSTER_MAX_CARDS) {
        strncpy(s_daily[s_dailyCount].uid, uid, sizeof(s_daily[s_dailyCount].uid) - 1);
        s_daily[s_dailyCount].uid[sizeof(s_daily[s_dailyCount].uid) - 1] = 0;
        s_daily[s_dailyCount].count = 1;
        s_dailyCount++;
        return 1;
    }
    return 1;
}

bool push(const Tap& t) {
    if (records() - s_cursor >= QUEUE_CAPACITY) return false;
    Rec r{};
    strncpy(r.uid, t.uid, sizeof r.uid - 1);
    strncpy(r.id,  t.id,  sizeof r.id  - 1);
    r.recorded_at = t.recorded_at;
    r.sms_sent    = t.sms_sent ? 1 : 0;
    File f = LittleFS.open(LOG_PATH, "a");
    if (!f) return false;
    bool ok = f.write((const uint8_t*)&r, sizeof r) == sizeof r;
    f.close();
    return ok;
}

size_t depth() { return records() - s_cursor; }

size_t peekBatch(Tap* out, size_t max) {
    File f = LittleFS.open(LOG_PATH, "r");
    if (!f) return 0;
    f.seek(s_cursor * sizeof(Rec));
    size_t n = 0;
    Rec r;
    while (n < max && f.read((uint8_t*)&r, sizeof r) == sizeof r) {
        strncpy(out[n].uid, r.uid, sizeof out[n].uid - 1);
        strncpy(out[n].id,  r.id,  sizeof out[n].id  - 1);
        out[n].recorded_at = r.recorded_at;
        out[n].sms_sent    = r.sms_sent != 0;
        n++;
    }
    f.close();
    return n;
}

void commit(size_t n) { s_cursor += n; writeCursor(s_cursor); }

}
