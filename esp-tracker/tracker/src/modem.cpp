#include "modem.h"
#include "../include/pins.h"
#include "../include/config.h"
#include <HardwareSerial.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

static HardwareSerial s_serial(2);

// UPDATE (2026-08-31): the TinyGsm + SSLClient + ArduinoHttpClient GPRS/TLS
// stack sketched below was never built past this comment — attach()/
// postJson() are still bare stubs — and it isn't going to be: this
// tracker's SIM800L is 2G-only, and 2G data is being shut off nationwide in
// the Philippines under an NTC mandate (complete by 2026-12-31). Confirmed
// on the scanner's IDENTICAL modem hardware, not assumed — see PLAN.md
// §1b's GPRS section for the full real-hardware diagnosis. Building a real
// HTTP/TLS uplink here would be building on a foundation already proven
// not to exist for this hardware+carrier. sendSms()/syncClockFromNetwork()
// below are implemented for real because AT+CMGS/AT+CCLK both work over
// plain network registration, independent of the GPRS attach that's the
// actual blocker — same distinction PLAN.md already draws for the
// scanner. See report.h for the SMS-based replacement for routine
// position uplink.
//   TinyGsm        modem(s_serial);
//   TinyGsmClient  raw(modem);
//   SSLClient      tls(&raw);        <-- crypto on the ESP32, not the SIM800L
//   HttpClient     http(tls, API_HOST, API_PORT);
// Pin the server cert/CA rather than shipping a full bundle: one endpoint,
// less flash, stronger guarantee.

// ---------------------------------------------------------------- AT core --
// Same technique as scanner-uno/sms_scanner/src/modem.cpp (proven on real
// SIM800L hardware this session) — a rolling window to detect a token
// without buffering a whole reply, kept here rather than shared because the
// two boards don't share a build (AVR SoftwareSerial vs. ESP32
// HardwareSerial) and duplicating ~40 lines is cheaper than inventing a
// cross-project shared library for it.
//
// KNOWN LIMITATION, same one documented for the scanner: every call below
// blocks the caller for one AT exchange, up to SMS_SEND_TIMEOUT_MS for a
// send. main.cpp's loop() calls sos::service() every iteration, so a
// button hold during a blocking modem call is only DELAYED until control
// returns (millis()-based, not lost) — not ideal for a safety device, but
// the same accepted tradeoff already made throughout this project rather
// than building a second FreeRTOS task to decouple them, which nothing
// here currently needs badly enough to justify.
static char s_win[16];
static void winPush(char c) { memmove(s_win, s_win + 1, sizeof(s_win) - 1); s_win[sizeof(s_win) - 1] = c; }
static bool winEnds(const char* s) {
    size_t n = strlen(s);
    if (n > sizeof(s_win)) return false;
    return memcmp(s_win + sizeof(s_win) - n, s, n) == 0;
}

static void atSend(const char* cmd) { s_serial.print(cmd); s_serial.print("\r\n"); }

static bool atWait(const char* token, uint32_t timeoutMs) {
    memset(s_win, 0, sizeof(s_win));
    uint32_t deadline = millis() + timeoutMs;
    while ((int32_t)(millis() - deadline) < 0) {
        if (s_serial.available()) {
            winPush((char)s_serial.read());
            if (winEnds(token)) return true;
        }
    }
    return false;
}

static bool atCmd(const char* cmd, const char* expect = "OK", uint32_t timeoutMs = 3000) {
    while (s_serial.available()) s_serial.read();
    atSend(cmd);
    return atWait(expect, timeoutMs);
}

// Reads raw bytes for the WHOLE timeoutMs window (or until buf is full) —
// deliberately NOT stopping at the first newline. The SIM800L's replies to
// AT+CCLK?/AT+CSQ both start with a blank "\r\n" before the line that
// actually matters — see scanner-uno/sms_scanner/src/modem.cpp's own
// comment on this exact bug, found and fixed on real hardware this
// session. Written from the start with that lesson rather than re-learning
// it here.
static uint8_t readFor(char* buf, size_t cap, uint32_t timeoutMs) {
    uint8_t n = 0;
    uint32_t deadline = millis() + timeoutMs;
    while ((int32_t)(millis() - deadline) < 0 && n < cap - 1) {
        if (s_serial.available()) buf[n++] = (char)s_serial.read();
    }
    buf[n] = 0;
    return n;
}

namespace modem {

bool begin() {
    pinMode(PIN_MODEM_EN, OUTPUT);
    pinMode(PIN_MODEM_PWRKEY, OUTPUT);
    pinMode(PIN_MODEM_DTR, OUTPUT);
    digitalWrite(PIN_MODEM_EN, HIGH);
    // PWRKEY: pulse LOW ~1s to bring the module up.
    digitalWrite(PIN_MODEM_PWRKEY, LOW);  delay(1100);
    digitalWrite(PIN_MODEM_PWRKEY, HIGH);
    s_serial.begin(9600, SERIAL_8N1, PIN_MODEM_RX, PIN_MODEM_TX);
    delay(3000);   // module boot time, same figure confirmed on the scanner's identical SIM800L

    bool ok = false;
    for (uint8_t i = 0; i < 5 && !ok; i++) ok = atCmd("AT", "OK", 2000);
    if (!ok) return false;
    atCmd("ATE0", "OK", 1000);         // stop echoing commands back
    atCmd("AT+CMGF=1", "OK", 1000);    // SMS text mode, set once
    return true;
}

// GPRS attach — deliberately left unimplemented, see the file header
// comment: confirmed dead on this hardware/carrier, not a TODO anyone
// should pick back up without re-testing that finding first.
bool attach()        { return false; }
bool attached()      { return false; }

void sleep() { digitalWrite(PIN_MODEM_DTR, HIGH); }   // AT+CSCLK=1 itself: TODO, needs attach() first to matter
void wake()  { digitalWrite(PIN_MODEM_DTR, LOW); delay(50); }

int  postJson(const char* path, const char* json) { (void)path;(void)json; return -1; } // see file header — no GPRS to send it over
void closeIdle()     { }                // no-op without attach()

bool syncClockFromNetwork() {
    while (s_serial.available()) s_serial.read();
    atSend("AT+CCLK?");
    // Response looks like: +CCLK: "24/08/29,14:03:11+32"
    char buf[48];
    readFor(buf, sizeof buf, 3000);
    char* q = strchr(buf, '"');
    if (!q) return false;
    int yy, mo, dd, hh, mi, ss, tzQuarter;
    if (sscanf(q + 1, "%d/%d/%d,%d:%d:%d+%d", &yy, &mo, &dd, &hh, &mi, &ss, &tzQuarter) < 6) return false;
    if (yy < 20) return false;   // "80/01/01..." is the modem's no-network-time default

    // Days-from-civil (Howard Hinnant's algorithm) — same as scanner-uno's
    // copy; the ESP32 has no RTC of its own to feed, this just disciplines
    // the system clock via settimeofday() so TLS validation (if it's ever
    // built) and this module's own timestamps agree with real time.
    int y = 2000 + yy;
    int m = mo, d = dd;
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097 + (long)doe - 719468;

    uint32_t epoch = (uint32_t)days * 86400UL + (uint32_t)hh * 3600UL + (uint32_t)mi * 60UL + (uint32_t)ss;
    epoch -= (uint32_t)tzQuarter * 900UL;   // NITZ offset is quarter-hours; convert to UTC

    struct timeval tv = { (time_t)epoch, 0 };
    settimeofday(&tv, nullptr);
    return true;
}

bool sendSms(const char* number, const char* text) {
    char cmd[32];
    snprintf(cmd, sizeof cmd, "AT+CMGS=\"%s\"", number);
    if (!atCmd(cmd, ">", 3000)) return false;
    s_serial.print(text);
    s_serial.write(0x1A);
    return atWait("OK", SMS_SEND_TIMEOUT_MS);
}

bool cellInfo(uint16_t& mcc, uint16_t& mnc, uint16_t& lac, uint32_t& cellId, int8_t& rssi) {
    (void)mcc;(void)mnc;(void)lac;(void)cellId;(void)rssi; return false;  // TODO: AT+CENG=1,1 — doesn't need GPRS, worth picking up separately from the HTTP stack above
}

int8_t signalQuality() {
    while (s_serial.available()) s_serial.read();
    atSend("AT+CSQ");
    char buf[32];
    readFor(buf, sizeof buf, 2000);
    char* p = strstr(buf, "+CSQ:");
    if (!p) return -1;
    int csq = atoi(p + 5);
    return (csq == 99) ? (int8_t)-1 : (int8_t)csq;
}

}
