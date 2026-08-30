#include "modem.h"
#include "roster.h"
#include "../include/pins.h"
#include "../include/config.h"
#include <Arduino.h>
#include <SoftwareSerial.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

static SoftwareSerial s_sim(PIN_SIM_RX, PIN_SIM_TX);
static bool s_attached = false;
static modem::IdleHook s_idleHook = nullptr;

static inline void idleTick() { if (s_idleHook) s_idleHook(); }

// ---------------------------------------------------------------- AT core --

static void atSend(const char* cmd) { s_sim.print(cmd); s_sim.print("\r\n"); }

// Rolling 8-byte window so we can test "did the stream just end with token"
// without buffering the whole reply — the only thing this needs to remember
// is whether a known literal (never longer than "ALREADY CONNECT") just
// appeared.
static char s_win[16];
static void winPush(char c) { memmove(s_win, s_win + 1, sizeof(s_win) - 1); s_win[sizeof(s_win) - 1] = c; }
static bool winEnds(const char* s) {
    size_t n = strlen(s);
    if (n > sizeof(s_win)) return false;
    return memcmp(s_win + sizeof(s_win) - n, s, n) == 0;
}

// Waits up to timeoutMs for `token` to appear in the stream. Returns true if
// seen. This is the blocking-with-timeout primitive the whole module is
// built on — see modem.h's note on what idleTick() buys back while it spins.
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

// -------------------------------------------------------- +IPD de-framing --
// SIM800L pushes inbound TCP data as "+IPD,<n>:<n bytes>", possibly split
// across several chunks for one reply. This hands the caller one content
// byte at a time, transparently spanning chunks, so nothing above this
// layer ever needs to buffer a whole HTTP response.

static int s_ipdRemaining = 0;

static void ipdReset() { s_ipdRemaining = 0; }

static int nextIpdByte(uint32_t deadline, bool& closed) {
    closed = false;
    for (;;) {
        if (s_ipdRemaining > 0) {
            while (!s_sim.available()) {
                if ((int32_t)(millis() - deadline) >= 0) return -1;
                idleTick();
            }
            s_ipdRemaining--;
            return s_sim.read();
        }
        // Between chunks: watch for the next "+IPD," header or "CLOSED".
        if ((int32_t)(millis() - deadline) >= 0) return -1;
        if (!s_sim.available()) { idleTick(); continue; }
        char c = (char)s_sim.read();
        winPush(c);
        if (winEnds("CLOSED")) { closed = true; return -1; }
        if (winEnds("+IPD,")) {
            char lenBuf[6]; uint8_t li = 0;
            for (;;) {
                if ((int32_t)(millis() - deadline) >= 0) return -1;
                if (!s_sim.available()) { idleTick(); continue; }
                char d = (char)s_sim.read();
                if (d == ':') break;
                if (li < sizeof(lenBuf) - 1) lenBuf[li++] = d;
            }
            lenBuf[li] = 0;
            s_ipdRemaining = atoi(lenBuf);
        }
    }
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

bool attached() { return s_attached; }

bool ensureAttached() {
    if (s_attached) return true;
    if (!GPRS_ENABLED) return false;   // see config.h — attach is confirmed
                                        // failing on the current signal, and
                                        // every real attempt costs up to
                                        // ~38s blocked before failing anyway
    // Clears any stale GPRS/PDP context left over from a previous attempt —
    // the modem itself doesn't power-cycle on an Arduino-side reset, so a
    // half-open context can survive and make CSTT fail every time until
    // this runs. Response varies ("SHUT OK" or "ERROR" if nothing was
    // open) so don't gate on it, just give it a moment either way.
    atCmd("AT+CIPSHUT", "SHUT OK", 3000);
    if (!atCmd("AT+CGATT=1", "OK", 5000)) return false;
    char cmd[48];
    snprintf(cmd, sizeof cmd, "AT+CSTT=\"%s\",\"%s\",\"%s\"", GPRS_APN, GPRS_USER, GPRS_PASS);
    if (!atCmd(cmd, "OK", 5000)) return false;
    if (!atCmd("AT+CIICR", "OK", 20000)) return false;
    if (!atCmd("AT+CIFSR", ".", 5000)) return false;
    s_attached = true;
    return true;
}

bool syncClockFromNetwork(uint32_t& epochOut) {
    while (s_sim.available()) s_sim.read();
    atSend("AT+CCLK?");
    // Response looks like: +CCLK: "24/08/29,14:03:11+32"
    char buf[40]; uint8_t n = 0;
    uint32_t deadline = millis() + 3000;
    while ((int32_t)(millis() - deadline) < 0 && n < sizeof(buf) - 1) {
        if (!s_sim.available()) { idleTick(); continue; }
        buf[n++] = (char)s_sim.read();
    }
    buf[n] = 0;
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

int request(const char* method, const char* path, const char* authBearer,
            const char* body, char* respBuf, size_t respCap) {
    if (!ensureAttached()) return -1;

    char start[48];
    snprintf(start, sizeof start, "AT+CIPSTART=\"TCP\",\"%s\",%u", API_HOST, (unsigned)API_PORT);
    atSend(start);

    if (!atWait("CONNECT OK", 8000) && !atWait("ALREADY CONNECT", 500)) {
        s_attached = false;   // socket-level failure — assume GPRS dropped, re-attach next time
        return -1;
    }

    if (!atCmd("AT+CIPSEND", ">", 3000)) { atCmd("AT+CIPCLOSE"); return -1; }

    s_sim.print(method); s_sim.print(' '); s_sim.print(path); s_sim.print(" HTTP/1.1\r\n");
    s_sim.print("Host: "); s_sim.print(API_HOST); s_sim.print("\r\n");
    if (authBearer) { s_sim.print("Authorization: Bearer "); s_sim.print(authBearer); s_sim.print("\r\n"); }
    if (body) {
        s_sim.print("Content-Type: application/json\r\n");
        s_sim.print("Content-Length: "); s_sim.print(strlen(body)); s_sim.print("\r\n");
    }
    s_sim.print("Connection: close\r\n\r\n");
    if (body) s_sim.print(body);
    s_sim.write(0x1A);   // Ctrl-Z ends the CIPSEND payload

    if (!atWait("SEND OK", 5000)) { atCmd("AT+CIPCLOSE"); s_attached = false; return -1; }

    ipdReset();
    uint32_t deadline = millis() + 8000;
    size_t n = 0;
    bool closed = false;
    for (;;) {
        int b = nextIpdByte(deadline, closed);
        if (b < 0) break;
        if (n < respCap - 1) respBuf[n++] = (char)b;
    }
    respBuf[n] = 0;
    atCmd("AT+CIPCLOSE", "CLOSE OK", 2000);   // best-effort; CLOSED already told us it's over

    if (!closed && n == 0) return -1;         // nothing came back at all — treat as failure
    int code = -1;
    if (strncmp(respBuf, "HTTP/1.", 7) == 0) code = atoi(respBuf + 9);
    return code;
}

bool requestRosterStream(const char* path, const char* authBearer) {
    if (!ensureAttached()) return false;

    char start[48];
    snprintf(start, sizeof start, "AT+CIPSTART=\"TCP\",\"%s\",%u", API_HOST, (unsigned)API_PORT);
    atSend(start);
    if (!atWait("CONNECT OK", 8000) && !atWait("ALREADY CONNECT", 500)) { s_attached = false; return false; }

    if (!atCmd("AT+CIPSEND", ">", 3000)) { atCmd("AT+CIPCLOSE"); return false; }
    s_sim.print("GET "); s_sim.print(path); s_sim.print(" HTTP/1.1\r\n");
    s_sim.print("Host: "); s_sim.print(API_HOST); s_sim.print("\r\n");
    if (authBearer) { s_sim.print("Authorization: Bearer "); s_sim.print(authBearer); s_sim.print("\r\n"); }
    s_sim.print("Connection: close\r\n\r\n");
    s_sim.write(0x1A);
    if (!atWait("SEND OK", 5000)) { atCmd("AT+CIPCLOSE"); s_attached = false; return false; }

    ipdReset();
    uint32_t deadline = millis() + 15000;   // a full roster can take longer than a small POST
    bool closed = false;

    // Pass 1 (implicit, inline below): skip the status line + headers up to
    // the blank line, checking the status code as we go.
    char statusLine[16]; uint8_t sl = 0;
    bool inHeaders = true;
    char crlf[4] = {0};   // rolling match for "\r\n\r\n"
    bool ok200 = false;

    roster::refreshBegin();
    char token[9]; uint8_t ti = 0;
    bool inQuote = false;

    for (;;) {
        int b = nextIpdByte(deadline, closed);
        if (b < 0) break;
        char c = (char)b;

        if (inHeaders) {
            if (sl < sizeof(statusLine) - 1) statusLine[sl++] = c;
            memmove(crlf, crlf + 1, 3); crlf[3] = c;
            if (memcmp(crlf, "\r\n\r\n", 4) == 0) {
                statusLine[sl] = 0;
                ok200 = (strncmp(statusLine, "HTTP/1.", 7) == 0 && atoi(statusLine + 9) == 200);
                inHeaders = false;
            }
            continue;
        }

        // Body: pull out each "xxxxxxxx" (8 hex chars) quoted token. This is
        // a hand-rolled scan for the KNOWN {"h":["..",".."]} shape, not a
        // general JSON parser — see roster.h.
        if (c == '"') {
            if (inQuote && ti == 8) { token[8] = 0; roster::refreshAdd(token); }
            inQuote = !inQuote;
            ti = 0;
        } else if (inQuote && ti < 8 && isxdigit((unsigned char)c)) {
            token[ti++] = c;
        }
    }

    if (ok200 && closed) { roster::refreshCommit(); return true; }
    return false;   // any failure leaves the previous cached roster untouched
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
    char buf[24]; uint8_t n = 0;
    uint32_t deadline = millis() + 2000;
    while ((int32_t)(millis() - deadline) < 0 && n < sizeof(buf) - 1) {
        if (!s_sim.available()) { idleTick(); continue; }
        buf[n++] = (char)s_sim.read();
    }
    buf[n] = 0;
    char* p = strstr(buf, "+CSQ:");
    if (!p) return -1;
    int csq = atoi(p + 5);
    return (csq == 99) ? (int8_t)-1 : (int8_t)csq;
}

}
