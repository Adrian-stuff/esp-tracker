#include "modem.h"
#include "../include/pins.h"
#include "../include/config.h"
#include <Arduino.h>
#include <SoftwareSerial.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static SoftwareSerial s_sim(PIN_SIM_RX, PIN_SIM_TX);
static modem::IdleHook s_idleHook = nullptr;

static inline void idleTick() { if (s_idleHook) s_idleHook(); }

// ---------------------------------------------------------------- AT core --

static void atSend(const char* cmd) { s_sim.print(cmd); s_sim.print("\r\n"); }

// Rolling window so "did the stream just end with token" can be tested
// without buffering the whole reply — same technique as ../src/modem.cpp,
// just no +IPD de-framing layered on top since there is no TCP socket here.
static char s_win[16];
static void winPush(char c) { memmove(s_win, s_win + 1, sizeof(s_win) - 1); s_win[sizeof(s_win) - 1] = c; }
static bool winEnds(const char* s) {
    size_t n = strlen(s);
    if (n > sizeof(s_win)) return false;
    return memcmp(s_win + sizeof(s_win) - n, s, n) == 0;
}

static bool atWait(const char* token, uint32_t timeoutMs) {
    memset(s_win, 0, sizeof(s_win));
    uint32_t deadline = millis() + timeoutMs;
    while ((int32_t)(millis() - deadline) < 0) {
        if (s_sim.available()) {
            winPush((char)s_sim.read());
            if (winEnds(token)) return true;
        }
        idleTick();
    }
    return false;
}

static bool atCmd(const char* cmd, const char* expect = "OK", uint32_t timeoutMs = 3000) {
    while (s_sim.available()) s_sim.read();   // drain anything stale first
    atSend(cmd);
    return atWait(expect, timeoutMs);
}

// Reads raw bytes into buf for the WHOLE timeoutMs window (or until buf is
// full) — used by registered()/signalQuality() to capture a reply for
// parsing, same pattern already proven by syncClockFromNetwork() below.
// Deliberately does NOT stop at the first '\n': the SIM800L's replies to
// AT+CREG?/AT+CSQ both start with a blank "\r\n" before the line that
// actually matters ("+CREG: 0,1", "+CSQ: 14,99") — an earlier version of
// this function stopped there, which meant registered()/signalQuality()
// never saw the real content and reported the modem as unreachable when it
// was answering fine (confirmed by real SMS sends working throughout).
static uint8_t readFor(char* buf, size_t cap, uint32_t timeoutMs) {
    uint8_t n = 0;
    uint32_t deadline = millis() + timeoutMs;
    while ((int32_t)(millis() - deadline) < 0 && n < cap - 1) {
        if (!s_sim.available()) { idleTick(); continue; }
        buf[n++] = (char)s_sim.read();
    }
    buf[n] = 0;
    return n;
}

// ------------------------------------------------------------- public API --

namespace modem {

void setIdleHook(IdleHook hook) { s_idleHook = hook; }

bool begin() {
    s_sim.begin(9600);   // confirmed against real hardware — see pins.h
    delay(3000);   // module boot time — this breakout auto-powers on (PWRKEY tied to GND), no toggle needed
    bool ok = false;
    for (uint8_t i = 0; i < 5 && !ok; i++) ok = atCmd("AT", "OK", 2000);
    if (!ok) return false;
    atCmd("ATE0", "OK", 1000);         // stop echoing commands back — halves what we have to parse
    atCmd("AT+CMGF=1", "OK", 1000);    // SMS text mode, set once
    return true;
}

bool registered() {
    while (s_sim.available()) s_sim.read();
    atSend("AT+CREG?");
    // Response looks like: +CREG: 0,1  (n,stat) — stat 1=home, 5=roaming.
    char buf[32];
    readFor(buf, sizeof buf, 2000);
    char* p = strstr(buf, "+CREG:");
    if (!p) return false;
    char* comma = strchr(p, ',');
    if (!comma) return false;
    int stat = atoi(comma + 1);
    return stat == 1 || stat == 5;
}

bool syncClockFromNetwork(uint32_t& epochOut) {
    while (s_sim.available()) s_sim.read();
    atSend("AT+CCLK?");
    // Response looks like: +CCLK: "24/08/29,14:03:11+32"
    char buf[48];
    readFor(buf, sizeof buf, 3000);
    char* q = strchr(buf, '"');
    if (!q) return false;
    int yy, mo, dd, hh, mi, ss, tzQuarter;
    if (sscanf(q + 1, "%d/%d/%d,%d:%d:%d+%d", &yy, &mo, &dd, &hh, &mi, &ss, &tzQuarter) < 6) return false;
    if (yy < 20) return false;   // "80/01/01..." is the modem's no-network-time default

    // Days-from-civil (Howard Hinnant's algorithm) — no <time.h> mktime on
    // AVR worth trusting for this, and this is ~10 lines either way.
    int y = 2000 + yy;
    int m = mo, d = dd;
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097 + (long)doe - 719468;   // days since 1970-01-01

    uint32_t epoch = (uint32_t)days * 86400UL + (uint32_t)hh * 3600UL + (uint32_t)mi * 60UL + (uint32_t)ss;
    epoch -= (uint32_t)tzQuarter * 900UL;   // NITZ offset is quarter-hours; convert to UTC
    epochOut = epoch;
    return true;
}

bool sendSms(const char* number, const char* text) {
    char cmd[32];
    snprintf(cmd, sizeof cmd, "AT+CMGS=\"%s\"", number);
    if (!atCmd(cmd, ">", 3000)) return false;
    s_sim.print(text);
    s_sim.write(0x1A);
    return atWait("OK", SMS_SEND_TIMEOUT_MS);
}

int8_t signalQuality() {
    while (s_sim.available()) s_sim.read();
    atSend("AT+CSQ");
    char buf[32];
    readFor(buf, sizeof buf, 2000);
    char* p = strstr(buf, "+CSQ:");
    if (!p) return -1;
    int csq = atoi(p + 5);
    return (csq == 99) ? (int8_t)-1 : (int8_t)csq;
}

bool pollSms(char* fromOut, size_t fromCap, char* textOut, size_t textCap) {
    while (s_sim.available()) s_sim.read();
    atSend("AT+CMGL=\"REC UNREAD\"");
    // A reply listing one message looks like:
    //   +CMGL: 1,"REC UNREAD","+639171234567",,"24/08/29,14:03:11+32"
    //   <message body>
    //   OK
    // Sized for one header line plus a near-full single-SMS body
    // (SMS_RX_BODY_MAX) plus the trailing OK — see that constant's
    // comment in config.h.
    char buf[200];
    readFor(buf, sizeof buf, 3000);

    char* p = strstr(buf, "+CMGL:");
    if (!p) return false;   // nothing unread
    int index = atoi(p + 6);

    // The header has THREE quoted fields before the sender: "REC UNREAD"
    // is the first; the sender's number is the second.
    char* q = strchr(p, '"');
    if (!q) return false;
    q = strchr(q + 1, '"');   // end of "REC UNREAD"
    if (!q) return false;
    q = strchr(q + 1, '"');   // opening quote of the sender number
    if (!q) return false;
    q++;
    size_t n = 0;
    while (*q && *q != '"' && n < fromCap - 1) fromOut[n++] = *q++;
    fromOut[n] = 0;

    // Body is the line right after the header's terminating CRLF — text
    // mode never wraps a single-part SMS across more lines than that.
    char* bodyStart = strstr(p, "\r\n");
    if (!bodyStart) return false;
    bodyStart += 2;
    n = 0;
    while (*bodyStart && *bodyStart != '\r' && *bodyStart != '\n' && n < textCap - 1) textOut[n++] = *bodyStart++;
    textOut[n] = 0;

    // Delete it so the next poll doesn't see the same message again.
    char cmd[16];
    snprintf(cmd, sizeof cmd, "AT+CMGD=%d", index);
    atCmd(cmd, "OK", 2000);

    return true;
}

}
