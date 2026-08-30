#include <Arduino.h>
#include <Wire.h>
#include "../include/pins.h"
#include "../include/config.h"
#include "reader.h"
#include "card.h"
#include "clock.h"
#include "store.h"
#include "modem.h"
#include "smsq.h"
#include "display.h"
#include "buzzer.h"
#include <avr/wdt.h>
#include <string.h>

// Some Uno clone bootloaders leave the watchdog armed with a short timeout;
// without an early disable, nothing pets it and the board resets partway
// through setup(), repeatably, looking exactly like a hardware fault. This
// runs in .init3 — before main()/setup(), even before C++ global
// constructors — the earliest point that matters if the timeout is short.
// Copied from ../src/main.cpp, which found this the hard way — see PLAN.md §1b.
void wdtEarlyDisable(void) __attribute__((naked)) __attribute__((section(".init3")));
void wdtEarlyDisable(void) {
    MCUSR = 0;
    wdt_disable();
}

// SMS-only gate scanner — see ../README.md for why this firmware exists
// separately from ../src/ rather than as a flag on it. Two rules, same as
// every build in this project:
//
//   1. A TAP IS ACCEPTED EVEN WHEN THE MODEM IS DOWN. reader::poll() and
//      store::push() never touch the modem — only smsq::service() does,
//      on its own schedule below.
//
//   2. NOTHING BLOCKS THE READER FOR LONG. modem::sendSms() is
//      bounded-blocking (see modem.h) — a tap during a send waits for it
//      to return, worst case SMS_SEND_TIMEOUT_MS. See idleDuringModemWait()
//      below for the one thing that stays responsive even during that wait.
//
// There is no roster, no server ingest, and no gateway-mode SMS here — see
// ../README.md's "What this build deliberately does not do" section. Every
// notification comes straight from the phone number written on the card
// (card.h) by card_writer/.

static uint32_t s_lastClock = 0;
static bool     s_awaitingSmsDone = false;   // an SMS was just enqueued — waiting
                                              // for smsq to actually send it so the
                                              // LCD can stop showing "Sending..."
static uint32_t s_idleAt = 0;                // when to revert the LCD to Status::Idle;
                                              // 0 = no revert pending.

// Shows a RESULT (as opposed to the transient "Sending..." state) and arms
// the revert-to-idle timer — see STATUS_HOLD_MS in config.h.
static void showResult(Status s, const char* detail) {
    display::show(s, detail);
    s_idleAt = millis() + STATUS_HOLD_MS;
}

// Handles one tap if the reader has one ready. A standalone function only
// so it reads cleanly from loop() — see idleDuringModemWait()'s comment
// for why it does NOT run during a blocking modem call.
static void handleTap() {
    Tap t;
    if (reader::poll(t)) {
        // Cancel any revert-to-idle timer left over from a PREVIOUS tap's
        // result before showing anything for this one — a stale timer from
        // an earlier tap firing mid-"Sending..." on a later tap would snap
        // the LCD back to "Ready" while a real send was still in flight.
        // Every new tap owns the timer outright from the moment it's
        // detected.
        s_idleAt = 0;

        if (!clockw::ok()) {
            buzzer::play(Cue::Error);
            showResult(Status::Error, "No RTC clock");
            reader::release();
        } else {
            // Read once, up front, while the card is still SELECTED
            // (reader::poll() deferred the halt exactly so this could
            // happen) — this MUST run before reader::release() below.
            CardData cd;
            bool hasCardData = card::read(cd);
            const char* label = hasCardData ? cd.name : t.uid;

            store::push(t.uid, t.recorded_at, hasCardData);   // local audit trail — always logged, see store.h

            if (hasCardData) {
                char msg[SMS_BODY_MAX];
                snprintf(msg, sizeof msg, "%s tapped in", cd.name);
                if (smsq::enqueue(cd.phone, msg)) {
                    buzzer::play(Cue::Accepted);
                    display::show(Status::Sending, label);   // see loop()'s s_awaitingSmsDone; no idle timer armed here on purpose, only on the terminal result below
                    s_awaitingSmsDone = true;
                } else {
                    buzzer::play(Cue::Error);
                    showResult(Status::Error, "SMS busy");   // previous send still in flight — SMS_QUEUE_DEPTH=1
                }
            } else {
                buzzer::play(Cue::Unknown);
                showResult(Status::Unknown, "No card data");   // card not enrolled — see card_writer/
            }

            reader::release();
        }

        Serial.print(F("tap ")); Serial.print(t.uid);
        Serial.print(F(" logged=")); Serial.println((unsigned)store::depth());
    } else if (reader::sawDuplicate()) {
        buzzer::play(Cue::Duplicate);
    }
}

// modem::setIdleHook() target — called on every spin of every blocking AT
// wait. Only buzzer::service() runs here, not handleTap(): nesting a real
// SPI round trip (card::read()'s raw[48] buffer, the Tap struct, the SMS
// body) inside modem.cpp's own AT-wait call stack was tried in ../src/ and
// reverted after it reset the board on real hardware — this build starts
// from that lesson rather than re-learning it. See ../src/main.cpp's
// longer comment on the same function for the full story.
static void idleDuringModemWait() {
    buzzer::service();
}

// --------------------------------------------------------- serial console --
// A USB connection is the only way to get taps back off this device (see
// store.h) or to check registration/signal without waiting for a boot log
// to scroll by — deliberately just three commands, not a general shell.
static char    s_line[16];
static uint8_t s_lineLen = 0;

static void printDumpRow(const char* uid, uint32_t at, bool hadCardData) {
    Serial.print(uid);
    Serial.print(',');
    Serial.print(at);
    Serial.print(',');
    Serial.println(hadCardData ? F("card") : F("uid-only"));
}

static void handleConsoleLine() {
    if (strcmp(s_line, "DUMP") == 0) {
        Serial.print(F("uid,recorded_at,source (")); Serial.print((unsigned)store::depth()); Serial.println(F(" records)"));
        store::forEach(printDumpRow);
    } else if (strcmp(s_line, "CLEAR") == 0) {
        store::clear();
        Serial.println(F("OK: audit ring cleared"));
    } else if (strcmp(s_line, "SIG") == 0) {
        Serial.print(F("registered=")); Serial.print(modem::registered());
        Serial.print(F(" csq="));       Serial.println(modem::signalQuality());
    } else if (s_lineLen) {
        Serial.println(F("ERR: commands are DUMP, CLEAR, SIG"));
    }
}

static void serviceConsole() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            s_line[s_lineLen] = 0;
            handleConsoleLine();
            s_lineLen = 0;
            continue;
        }
        if (s_lineLen < sizeof(s_line) - 1) s_line[s_lineLen++] = c;
    }
}

void setup() {
    wdt_disable();   // belt-and-suspenders alongside the .init3 version above
    Serial.begin(9600);
    Wire.begin();

    display::begin();
    buzzer::begin();
    // display::begin() already left "Initializing" on line 1 — this build
    // keeps that up (with a line-2 detail of whatever's currently blocking
    // setup()) through the WHOLE init sequence below, not just an instant
    // before getting overwritten. modem::begin() alone can block several
    // seconds retrying AT — showing "Ready" before that finished (the
    // previous behavior: display::show(Status::Idle) ran right here) was
    // misleading, since a tap during that window works fine (nothing here
    // touches the modem) but the screen claimed the device was idle while
    // setup() hadn't actually returned yet.
    display::show(Status::Init, "RTC/reader...");

    bool rtc = clockw::begin();
    if (!rtc) Serial.println(F("FATAL: DS1302 missing, unset, or not answering - taps will be REFUSED"));

    store::begin();
    reader::begin();

    display::show(Status::Init, "Modem...");

    // Registered before modem::begin() runs so even boot-time blocking (the
    // AT ping retries in begin()) stays tap-responsive.
    modem::setIdleHook(idleDuringModemWait);
    bool modemOk = modem::begin();
    smsq::begin();

    bool reg = false;
    if (modemOk) {
        uint32_t epoch;
        if (modem::syncClockFromNetwork(epoch)) { clockw::setFromEpoch(epoch); s_lastClock = millis(); }
        reg = modem::registered();
    }

    display::show(Status::Idle, "");   // "" clears line 2 — setup() is genuinely done now

    Serial.print(F("sms_scanner up | rtc=")); Serial.print(clockw::ok());
    Serial.print(F(" logged="));              Serial.print((unsigned)store::depth());
    Serial.print(F(" modem="));               Serial.print(modemOk ? F("ok") : F("FAILED"));
    Serial.print(F(" net="));                 Serial.println(reg ? F("registered") : F("NOT REGISTERED - SMS will fail"));
    Serial.println(F("console: DUMP, CLEAR, SIG"));
}

void loop() {
    uint32_t now = millis();

    buzzer::service();
    smsq::service();
    serviceConsole();

    // SMS_QUEUE_DEPTH is 1, so "back to empty" reliably means the tap we
    // just logged has been sent (or given up on, if the modem itself
    // failed) — replace "Sending..." with a real confirmation instead of
    // leaving it on screen forever.
    if (s_awaitingSmsDone && smsq::depth() == 0) {
        showResult(Status::Ok, "SMS Sent");
        s_awaitingSmsDone = false;
    }

    handleTap();

    // Revert the LCD to "Ready" once a result has been on screen long
    // enough to read — see STATUS_HOLD_MS in config.h.
    if (s_idleAt && (int32_t)(now - s_idleAt) >= 0) {
        display::show(Status::Idle, "");   // "" (not nullptr) so display::show() blanks line 2 too, not just line 1 — see display.cpp's writeRow()
        s_idleAt = 0;
    }

    if (now - s_lastClock >= CLOCK_RESYNC_S * 1000UL) {
        s_lastClock = now;
        uint32_t epoch;
        if (modem::syncClockFromNetwork(epoch)) clockw::setFromEpoch(epoch);
    }
}
