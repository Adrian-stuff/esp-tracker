#include "modem.h"
#include "../include/pins.h"
#include "../include/config.h"
#include "store.h"   // store::depth() — wakes early when there's a backlog to drain
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

// Blackout detector — every AT exchange in this file funnels through
// atWait(), so this is the one place that sees every timeout regardless of
// which higher-level call it came from. A SIM800L brownout mid-TX-burst
// (see AGENTS.md's power warning — 2A bursts, needs a bulk cap right at the
// module) is exactly the failure mode this exists to surface: the module
// resets or hangs, every AT command after that times out identically, and
// without this nothing would distinguish "briefly out of coverage" from
// "the modem fell off the bus and isn't coming back without a power cycle".
static uint8_t  s_consecutiveFail = 0;
static bool     s_blackout        = false;
static constexpr uint8_t BLACKOUT_THRESHOLD = 3;

// Most recent sendSms() failure reason, human-readable — set alongside the
// Serial log line, and exposed via modem::lastError() so a caller that
// isn't watching the serial console (e.g. the BLE SEND command) can show
// the same detail instead of just "failed, go check the log".
static char s_lastError[80] = "";

// ---------------------------------------------------------- power cycle --
// Shared between sendSms()'s own self-wake/self-sleep and the
// servicePowerCycle() scheduler below — see config.h's
// MODEM_CFUN_IDLE_ENABLED block for the brownout problem this exists to
// mitigate. s_radioAwake is the single source of truth for "is CFUN
// currently 1 right now" that both consult so they don't fight each other
// (e.g. the scheduler won't open a second window if sendSms() already has
// one open for an outbound send, and vice versa).
static bool     s_radioAwake      = false;
enum class RadioState : uint8_t { Idle, WaitReg, Window };
static RadioState s_radioState      = RadioState::Idle;
static uint32_t   s_stateSince      = 0;
static uint32_t   s_lastWindowClose = 0;
static uint32_t   s_lastPeriodicWake = 0;
static uint32_t   s_lastRegPoll     = 0;
static bool       s_urgentRequest   = false;

static bool atWait(const char* token, uint32_t timeoutMs) {
    memset(s_win, 0, sizeof(s_win));
    uint32_t deadline = millis() + timeoutMs;
    while ((int32_t)(millis() - deadline) < 0) {
        if (s_serial.available()) {
            winPush((char)s_serial.read());
            if (winEnds(token)) {
                if (s_consecutiveFail) {
                    Serial.printf("[modem] responding again after %u failed command(s)\n", s_consecutiveFail);
                    s_consecutiveFail = 0;
                    s_blackout = false;
                }
                return true;
            }
        }
    }
    Serial.printf("[modem] AT timeout waiting for \"%s\" (%lums)\n", token, (unsigned long)timeoutMs);
    s_consecutiveFail++;
    if (!s_blackout && s_consecutiveFail >= BLACKOUT_THRESHOLD) {
        s_blackout = true;
        Serial.printf(
            "[modem] *** SIM800L BLACKOUT SUSPECTED *** %u consecutive AT timeouts — "
            "module not responding. Check LiPo connection and the bulk cap at the "
            "module (brownout during a TX burst is the classic cause; see AGENTS.md).\n",
            s_consecutiveFail);
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
    Serial.println("[modem] powering on SIM800L...");
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
    for (uint8_t i = 0; i < 5 && !ok; i++) {
        Serial.printf("[modem] AT probe %u/5...\n", i + 1);
        ok = atCmd("AT", "OK", 2000);
    }
    if (!ok) {
        Serial.println("[modem] *** NO RESPONSE FROM SIM800L AFTER 5 PROBES *** "
                        "check power/wiring — see AGENTS.md's power warning.");
        return false;
    }
    Serial.println("[modem] SIM800L responding, configuring...");
    atCmd("ATE0", "OK", 1000);         // stop echoing commands back
    atCmd("AT+CMGF=1", "OK", 1000);    // SMS text mode, set once
    // Radio is at its power-on default (CFUN=1) here — deliberately left
    // that way so setup()'s syncClockFromNetwork() (needs NITZ, which
    // needs registration) has a chance to work. main.cpp calls
    // modem::enterIdle() once, right after that attempt, to drop into the
    // CFUN=4 baseline — see config.h's MODEM_CFUN_IDLE_ENABLED block.
    s_radioAwake = true;
    Serial.println("[modem] ready");
    return true;
}

// GPRS attach — deliberately left unimplemented, see the file header
// comment: confirmed dead on this hardware/carrier, not a TODO anyone
// should pick back up without re-testing that finding first.
bool attach()        { return false; }
bool attached()      { return false; }

void sleep() {
    // AT+CSCLK=1 enables sleep mode. DTR HIGH = sleep, LOW = wake.
    // The modem stays attached while sleeping — a cold GSM attach costs 5-15s,
    // and for a safety device latency beats runtime.
    digitalWrite(PIN_MODEM_DTR, HIGH);
    if (MODEM_USE_CSCLK) atCmd("AT+CSCLK=1", "OK", 1000);
}

void wake() {
    digitalWrite(PIN_MODEM_DTR, LOW);
    delay(50);
    // Send a dummy AT to wake the modem from CSCLK sleep
    atCmd("AT", "OK", 1000);
}

// True once BLACKOUT_THRESHOLD consecutive AT commands have timed out —
// see atWait() above. Stays true until an AT command succeeds again.
bool blackout() { return s_blackout; }

// Human-readable reason the last sendSms() call failed, or "" if it
// succeeded (or nothing has been sent yet). See sendSms() for what sets it.
const char* lastError() { return s_lastError; }

// Explicit one-time transition from the modem's power-on default (CFUN=1,
// left that way by begin() so setup()'s syncClockFromNetwork() gets a
// chance to register) into the CFUN=4 idle baseline. Call once, after that
// clock-sync attempt, before entering loop().
void enterIdle() {
    if (!MODEM_CFUN_IDLE_ENABLED) return;
    Serial.println("[modem] entering CFUN=4 idle baseline");
    atCmd("AT+CFUN=4", "OK", 3000);
    s_radioAwake = false;
    s_radioState = RadioState::Idle;
    s_lastWindowClose = millis();
}

void forceWindow() { s_urgentRequest = true; }
bool radioReady()  { return s_radioAwake; }

static void closeWindow(const char* why) {
    delay(MODEM_AT_GAP_MS);
    Serial.printf("[modem] power-cycle: %s — CFUN=4\n", why);
    atCmd("AT+CFUN=4", "OK", 3000);
    s_radioAwake       = false;
    s_radioState       = RadioState::Idle;
    s_lastWindowClose  = millis();
}

// Non-blocking CFUN duty-cycle scheduler — see config.h's
// MODEM_CFUN_IDLE_ENABLED block. Call every loop() iteration. Distinct
// from sendSms()'s own self-wake/self-sleep: this one periodically opens a
// window to check for INCOMING SMS (a message can only be delivered while
// registered — there is no way to "poll for it" while sitting in CFUN=4),
// and gives any store.cpp backlog a chance to drain without each queued
// item individually paying the registration wait sendSms() would
// otherwise incur on its own.
//
// NOTE ON "non-blocking": the WAITING (for the timer, for registration, for
// the window to elapse) is fully non-blocking, spread across many loop()
// calls via millis(). The actual instant of sending "AT+CFUN=1"/"AT+CFUN=4"
// is still one short, bounded AT round trip (typically well under a
// second) — same as every other AT command in this file. Making that one
// instant non-blocking too would mean rewriting the whole UART layer
// around an async state machine, which nothing else here does either.
void servicePowerCycle() {
    if (!MODEM_CFUN_IDLE_ENABLED) return;
    uint32_t now = millis();

    switch (s_radioState) {
    case RadioState::Idle: {
        if (s_radioAwake) return;   // sendSms() already has the radio up — nothing to do

        bool cooldownOver = (now - s_lastWindowClose) >= MODEM_MIN_COOLDOWN_MS;
        if (!s_urgentRequest && !cooldownOver) return;

        bool periodicDue  = (now - s_lastPeriodicWake) >= MODEM_WAKE_INTERVAL_MS;
        bool queuePending = store::depth() > 0;
        if (!s_urgentRequest && !periodicDue && !queuePending) return;

        Serial.printf("[modem] power-cycle: opening a window (%s) — CFUN=1\n",
                      s_urgentRequest ? "urgent request" : periodicDue ? "periodic check" : "queue backlog");
        atCmd("AT+CFUN=1", "OK", 5000);
        s_radioAwake = true;
        if (periodicDue) s_lastPeriodicWake = now;
        delay(MODEM_AT_GAP_MS);
        s_radioState = RadioState::WaitReg;
        s_stateSince = now;
        return;
    }

    case RadioState::WaitReg: {
        if (now - s_lastRegPoll >= 1000) {   // poll at most once/sec — non-blocking
            s_lastRegPoll = now;
            int8_t stat = networkStatus();
            if (stat == 1 || stat == 5) {
                Serial.println("[modem] power-cycle: registered — window open");
                s_urgentRequest = false;
                s_radioState = RadioState::Window;
                s_stateSince = now;
                return;
            }
        }
        if (now - s_stateSince >= MODEM_REG_TIMEOUT_MS) {
            s_urgentRequest = false;
            closeWindow("registration timed out");
        }
        return;
    }

    case RadioState::Window: {
        // Nothing to actively do here: main.cpp polls pollSmsCommand()
        // while radioReady() is true, and store::drain() (already gated on
        // sos::smsIdle()) runs from loop() regardless — it'll find
        // s_radioAwake already true via sendSms() and just use this open
        // window instead of opening its own. This state just holds the
        // window open until its time is up.
        if (now - s_stateSince >= MODEM_WAKE_WINDOW_MS) {
            closeWindow("window elapsed");
        }
        return;
    }
    }
}

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

// While blackout() is true, sendSms() below fails almost instantly instead
// of repeating the full ~19s wake/register/send/sleep sequence — see its
// own comment. This is how often it still tries a FULL attempt anyway, to
// notice the modem coming back.
static constexpr uint32_t BLACKOUT_PROBE_INTERVAL_MS = 60000;
static uint32_t s_lastBlackoutProbe = 0;

bool sendSms(const char* number, const char* text, uint32_t timeoutMs, uint32_t regTimeoutMs) {
    // FAIL FAST if the modem is already confirmed unresponsive (blackout()
    // — 3+ consecutive AT timeouts already seen). CONFIRMED REAL PROBLEM,
    // not theoretical: without this, every store::drain() retry of a
    // queued Telemetry/SOS entry (roughly every 2s+ while backoff is still
    // small) repeats the FULL wake sequence below regardless of whether
    // the modem is even there — CFUN=1 attempt (~5s) + registration poll
    // (up to regTimeoutMs) + AT+CMGS attempt (~3s) + CFUN=4 close (~3s),
    // all of which time out identically when nothing answers. That is up
    // to ~19-27s of loop() blocked on a doomed attempt, repeating every
    // backoff cycle — during which serviceButton() never runs, so an
    // entire 2-second SOS press-and-release can happen in the gap and
    // never be sampled at all. Skip the whole dance most of the time once
    // blackout is confirmed; still try a full attempt every
    // BLACKOUT_PROBE_INTERVAL_MS so recovery is noticed.
    if (MODEM_CFUN_IDLE_ENABLED && !s_radioAwake && s_blackout) {
        uint32_t now = millis();
        if (now - s_lastBlackoutProbe < BLACKOUT_PROBE_INTERVAL_MS) {
            snprintf(s_lastError, sizeof s_lastError, "modem in blackout — skipped wake, failing fast");
            Serial.printf("[modem] sendSms to %s: skipped — blackout suspected, next full retry in ~%lus\n",
                          number, (unsigned long)((BLACKOUT_PROBE_INTERVAL_MS - (now - s_lastBlackoutProbe)) / 1000));
            return false;
        }
        s_lastBlackoutProbe = now;
        Serial.println("[modem] sendSms: blackout probe interval elapsed — trying a full wake anyway");
    }

    // Wake the radio ourselves if it's idle — see config.h's
    // MODEM_CFUN_IDLE_ENABLED block. selfWoke tracks whether THIS call is
    // the one that has to put it back to sleep afterward: if a
    // servicePowerCycle() window (or another sendSms() call, shouldn't
    // overlap in this single-threaded design but just in case) already has
    // the radio up, we use it and leave closing it to whoever opened it.
    bool selfWoke = false;
    if (MODEM_CFUN_IDLE_ENABLED && !s_radioAwake) {
        selfWoke = true;
        Serial.println("[modem] sendSms: radio idle — waking (AT+CFUN=1) before sending...");
        atCmd("AT+CFUN=1", "OK", 5000);
        s_radioAwake = true;   // RF is up now regardless of registration status yet
        // Whatever forceWindow() was asked for (see sos::trigger()) is now
        // moot: we're waking the radio right now ourselves. This MUST be
        // cleared here — servicePowerCycle()'s WaitReg state is the only
        // other place that clears it, and if sendSms() (as it almost always
        // does, since SOS calls forceWindow() then sendSms() moments later)
        // gets to the radio first, the scheduler's Idle case never reaches
        // WaitReg at all. Without this line the flag stays stuck true
        // forever, and the scheduler's own "urgent requests bypass the
        // cooldown" logic then reopens a window on every single Idle check
        // with NO cooldown — a tight CFUN=1/CFUN=4 loop after every SOS,
        // which is worse than the brownout problem this all exists to fix.
        s_urgentRequest = false;
        delay(MODEM_AT_GAP_MS);

        uint32_t start = millis();
        int8_t stat = -1;
        while ((int32_t)(millis() - (start + regTimeoutMs)) < 0) {
            stat = networkStatus();
            if (stat == 1 || stat == 5) break;   // 1=home, 5=roaming
            delay(MODEM_AT_GAP_MS);
        }
        Serial.printf("[modem] sendSms: %s after %lums\n",
                      (stat == 1 || stat == 5) ? "registered" : "registration timed out, trying anyway",
                      (unsigned long)(millis() - start));
    }

    Serial.printf("[modem] sending SMS to %s (%u bytes, timeout %lums): %s\n",
                  number, (unsigned)strlen(text), (unsigned long)timeoutMs, text);
    char cmd[32];
    snprintf(cmd, sizeof cmd, "AT+CMGS=\"%s\"", number);
    bool ok;
    if (!atCmd(cmd, ">", 3000)) {
        snprintf(s_lastError, sizeof s_lastError,
                 "no prompt from modem%s", s_blackout ? " (blackout suspected)" : "");
        Serial.printf("[modem] SMS to %s FAILED — %s\n", number, s_lastError);
        ok = false;
    } else {
        delay(MODEM_AT_GAP_MS);   // let the bulk cap recover before the TX-heavy body+Ctrl-Z
        s_serial.print(text);
        s_serial.write(0x1A);
        ok = atWait("OK", timeoutMs);
        if (!ok) {
            snprintf(s_lastError, sizeof s_lastError,
                     "no OK confirmation after send (signal/network/credit?)%s",
                     s_blackout ? " (blackout suspected)" : "");
        } else {
            s_lastError[0] = '\0';
        }
        Serial.printf("[modem] SMS to %s %s\n", number, ok ? "sent OK" : "FAILED — no OK from modem");
    }

    // Aggressive sleep fallback (config.h): the EXACT moment this send is
    // done — success or fail, doesn't matter — drop straight back to
    // CFUN=4, but only if this call is the one that woke the radio.
    if (MODEM_CFUN_IDLE_ENABLED && selfWoke) {
        delay(MODEM_AT_GAP_MS);
        Serial.println("[modem] sendSms: done — CFUN=4 (aggressive sleep fallback)");
        atCmd("AT+CFUN=4", "OK", 3000);
        s_radioAwake = false;
        s_lastWindowClose = millis();
    }
    return ok;
}

bool cellInfo(uint16_t& mcc, uint16_t& mnc, uint16_t& lac, uint32_t& cellId, int8_t& rssi) {
    // AT+CENG=1,1 enables cell engineer mode and returns serving + neighbor cells.
    // This does NOT require GPRS — it works on plain 2G registration.
    while (s_serial.available()) s_serial.read();
    atSend("AT+CENG=1,1");
    char buf[256];
    uint8_t n = readFor(buf, sizeof buf, 3000);

    // Response format: +CENG: <arfcn>,<rxlev>,<bsic>,<cellid>,<lac>,<mcc>,<mnc>,...
    // We only need the first line (serving cell).
    char* p = strstr(buf, "+CENG:");
    if (!p) return false;

    // Skip the +CENG: header and parse serving cell fields
    char* line = p + 6;
    while (*line == ' ') line++;

    // Parse: arfcn,rxlev,bsic,cellid,lac,mcc,mnc
    int arfcn, rxlev, bsic, cid, lacVal, mccVal, mncVal;
    if (sscanf(line, "%d,%d,%d,%d,%d,%d,%d",
               &arfcn, &rxlev, &bsic, &cid, &lacVal, &mccVal, &mncVal) < 7) return false;

    mcc = (uint16_t)mccVal;
    mnc = (uint16_t)mncVal;
    lac = (uint16_t)lacVal;
    cellId = (uint32_t)cid;
    // rxlev is received signal strength in dBm relative to -110 dBm.
    // Convert to approximate RSSI: rssi = rxlev - 110
    rssi = (int8_t)(rxlev - 110);
    return true;
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

int8_t networkStatus() {
    while (s_serial.available()) s_serial.read();
    atSend("AT+CREG?");
    char buf[48];
    readFor(buf, sizeof buf, 2000);
    char* p = strstr(buf, "+CREG:");
    if (!p) return -1;
    // Response: +CREG: <n>,<stat>[,<lac>,<ci>]
    int n, stat;
    if (sscanf(p + 6, "%d,%d", &n, &stat) < 2) return -1;
    return (int8_t)stat;
}

// SMS command polling — reads all unread SMS, processes config commands.
//
// Wire format: "<secret> SOS <number>" or "<secret> SCANNER <number>"
// The secret prevents anyone who learns the SIM number from reconfiguring
// the device. Reply SMS confirms what was set; the message is deleted after
// processing so it doesn't accumulate.
//
// This runs synchronously (one AT exchange per SMS) — acceptable because
// SMS commands are infrequent and the loop already blocks on sendSms().
bool pollSmsCommand(const char* secret,
                    void (*onSetSos)(const char*),
                    void (*onSetScanner)(const char*),
                    void (*onAck)(const char*)) {
    while (s_serial.available()) s_serial.read();
    atSend("AT+CMGL=\"REC UNREAD\"");
    char buf[512];
    uint8_t n = readFor(buf, sizeof buf, 4000);

    bool processed = false;
    char* p = buf;
    while ((p = strstr(p, "+CMGL:")) != nullptr) {
        // Parse: +CMGL: <idx>,"REC UNREAD","+<sender>","","+<date>,<time><tz>"
        int idx;
        char sender[24];
        if (sscanf(p + 6, "%d,\"REC UNREAD\",\"%23[^\"]\"", &idx, sender) < 2) { p++; continue; }

        // Find the body: starts after the next \r\n
        char* bodyStart = strstr(p, "\r\n");
        if (!bodyStart) { p++; continue; }
        bodyStart += 2;

        // Body ends at the next +CMGL or end of buffer
        char* bodyEnd = strstr(bodyStart, "+CMGL:");
        if (!bodyEnd) bodyEnd = buf + n;

        // Trim trailing whitespace
        while (bodyEnd > bodyStart && (*(bodyEnd - 1) == '\r' || *(bodyEnd - 1) == '\n' || *(bodyEnd - 1) == ' '))
            bodyEnd--;

        char body[256];
        size_t bodyLen = bodyEnd - bodyStart;
        if (bodyLen >= sizeof body) bodyLen = sizeof body - 1;
        memcpy(body, bodyStart, bodyLen);
        body[bodyLen] = '\0';

        // Check for secret prefix
        size_t secLen = strlen(secret);
        if (bodyLen > secLen && strncmp(body, secret, secLen) == 0) {
            const char* cmd = body + secLen;
            while (*cmd == ' ') cmd++;  // skip spaces
            Serial.printf("[modem] unread SMS from %s: command \"%s\"\n", sender, cmd);

            char reply[64];
            if (strncmp(cmd, "SOS ", 4) == 0) {
                const char* num = cmd + 4;
                while (*num == ' ') num++;
                onSetSos(num);
                snprintf(reply, sizeof reply, "SOS number set to %s", num);
            } else if (strncmp(cmd, "SCANNER ", 8) == 0) {
                const char* num = cmd + 8;
                while (*num == ' ') num++;
                onSetScanner(num);
                snprintf(reply, sizeof reply, "Scanner number set to %s", num);
            } else if (strncmp(cmd, "ACK ", 4) == 0 && onAck) {
                const char* id = cmd + 4;
                while (*id == ' ') id++;
                onAck(id);
                snprintf(reply, sizeof reply, "Acked %s", id);
            } else {
                snprintf(reply, sizeof reply, "Unknown cmd: %s", cmd);
            }

            // Reply and delete the processed message
            sendSms(sender, reply);
            char delCmd[16];
            snprintf(delCmd, sizeof delCmd, "AT+CMGD=%d", idx);
            atCmd(delCmd, "OK", 2000);
            processed = true;
        } else {
            // No secret prefix — not a command for us. Left as unread on
            // the SIM rather than deleted: this poll only recognizes
            // config/ack commands, so a stray text (wrong number, provider
            // notice, etc.) is silently ignored on every future poll too —
            // logged here so it's at least visible, not just silently
            // accumulating in SIM storage.
            Serial.printf("[modem] unread SMS from %s ignored (no command prefix): %.40s%s\n",
                          sender, body, bodyLen > 40 ? "..." : "");
        }

        p = bodyEnd;
    }
    return processed;
}

}
