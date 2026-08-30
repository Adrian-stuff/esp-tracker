#include <Arduino.h>
#include <Wire.h>
#include "../include/pins.h"
#include "../include/config.h"
#include "reader.h"
#include "card.h"
#include "clock.h"
#include "roster.h"
#include "store.h"
#include "modem.h"
#include "net.h"
#include "smsq.h"
#include "relay.h"
#include "display.h"
#include "buzzer.h"
#include <avr/wdt.h>

// Some Uno clone bootloaders leave the watchdog armed with a short timeout;
// without an early disable, nothing pets it and the board resets partway
// through setup(), repeatably, looking exactly like a hardware fault. This
// runs in .init3 — before main()/setup(), even before C++ global
// constructors — the earliest point that matters if the timeout is short.
// See scanner-uno/selftest/ and PLAN.md §1b for how this was found.
void wdtEarlyDisable(void) __attribute__((naked)) __attribute__((section(".init3")));
void wdtEarlyDisable(void) {
    MCUSR = 0;
    wdt_disable();
}

// Uno gate scanner — cellular-only. Same two rules as the ESP32 build:
//
//   1. A TAP IS ACCEPTED EVEN WHEN THE NETWORK IS DOWN. reader::poll(),
//      roster::known() and store::push() never touch the modem — only
//      net::drain()/relay::service() do, on their own timers below.
//
//   2. NOTHING BLOCKS THE READER FOR LONG. The modem calls inside
//      net::drain()/relay::service() ARE bounded-blocking (see modem.h) —
//      a tap during one of those calls waits for it to return, worst case
//      a few seconds. Tried making this fully non-blocking (modem.cpp
//      calling back into a tap handler on every spin of its AT waits) and
//      reverted it — see idleDuringModemWait()'s comment below for why:
//      it crashed the board on real hardware, not a hypothetical risk.
//      That's the one honest regression of this first-pass sketch.
//
// NOT present here: notify.cpp. The ESP32 build's SMS_DIRECT_MODE (one
// hardcoded family's number in flash) doesn't belong on a real multi-child
// gate — see that file's own warning. This build defaults straight to
// gateway mode: the server decides who gets texted from the tap it already
// received, and relay.cpp only relays already-composed messages.

static uint32_t s_lastDrain  = 0;
static uint32_t s_lastRoster = 0;
static uint32_t s_lastClock  = 0;
static bool     s_awaitingSmsDone = false;   // an offline fallback SMS was
                                              // just enqueued — waiting for
                                              // smsq to actually send it so
                                              // the LCD can stop showing
                                              // "Buffering..." forever

// Handles one tap if the reader has one ready. A standalone function only
// so it reads cleanly from loop() — it does NOT run during a blocking
// modem call (see idleDuringModemWait()'s comment for why that was tried
// and reverted).
static void handleTap() {
    Tap t;
    if (reader::poll(t)) {
        const bool known = roster::known(t.uid) || !roster::size();

        if (!clockw::ok()) {
            buzzer::play(Cue::Error);
            display::show(Status::Error, "No RTC clock");
            reader::release();
        } else {
            t.sms_sent = false;   // gateway mode: the server decides, not this device

            // Read once, up front, and reuse for both the LCD label and the
            // offline SMS below — the card is still SELECTED here
            // (reader::poll() deferred the halt exactly so this read could
            // happen), so this MUST run before reader::release() below. A
            // card with no offline payload written (card::read() fails)
            // falls back to showing the raw UID, same as before this
            // feature existed.
            CardData cd;
            bool hasCardData = card::read(cd);
            const char* label = hasCardData ? cd.name : t.uid;

            bool pushed  = store::push(t);
            bool offline = !net::online();

            if (!pushed)        { buzzer::play(Cue::Error);     display::show(Status::Error, "Queue full"); }
            else if (!known)    { buzzer::play(Cue::Unknown);   display::show(Status::Unknown, label); }
            else                { buzzer::play(offline ? Cue::Offline : Cue::Accepted);
                                  display::show(offline ? Status::Buffering : Status::Ok, label); }

            // Offline fallback: relay.cpp's server-driven SMS can't run
            // without a network, so text the parent directly using the
            // number read straight off the card. No ref passed — this is
            // direct mode, not a server-issued outbox job, so it must NOT
            // land in smsq's shared result queue (relay.cpp drains that
            // same queue for its own gateway-mode acks and would otherwise
            // try to ack a ref the server never issued). Completion is
            // tracked separately in loop() instead, via smsq::depth().
            if (pushed && offline && hasCardData) {
                char msg[SMS_BODY_MAX];
                snprintf(msg, sizeof msg, "%s tapped in (offline)", cd.name);
                smsq::enqueue(cd.phone, msg);
                s_awaitingSmsDone = true;
            }

            reader::release();
        }

        Serial.print(F("tap ")); Serial.print(t.uid);
        Serial.print(F(" queued=")); Serial.println((unsigned)store::depth());
    } else if (reader::sawDuplicate()) {
        buzzer::play(Cue::Duplicate);
    }
}

// modem::setIdleHook() target — called on every spin of every blocking AT
// wait.
//
// TRIED AND REVERTED: this used to also call handleTap() (throttled to
// every 50ms) so a card tapped mid-exchange would still get read and
// buffered. Confirmed on real hardware that this causes the board to
// reset — handleTap() nests a real SPI round trip and several local
// buffers (card::read()'s raw[48], the Tap struct, the SMS body) INSIDE
// modem.cpp's already-deep call stack (atWait() inside ensureAttached()/
// requestRosterStream(), which have their own locals), and this build has
// very little stack headroom left at 70%+ static RAM on a 2KB AVR — the
// same class of RAM-margin crash documented in PLAN.md §1b, just
// triggered by a different path. Only buzzer::service() is safe to keep
// here: it does no SPI, allocates nothing, and is mostly a backstop now
// anyway since buzzer::play() passes tone() a duration and the AVR core's
// own timer ISR stops it on schedule regardless of loop() being blocked.
//
// Net effect: the beep-during-a-blocking-send bug is fixed (via that tone()
// duration, not this hook), but a tap that arrives WHILE a GPRS/SMS AT
// exchange is in flight still has to wait for it to finish, same as
// before — see the top-of-file comment and modem.h's note on this.
static void idleDuringModemWait() {
    buzzer::service();
}

void setup() {
    wdt_disable();   // belt-and-suspenders alongside the .init3 version above
    Serial.begin(9600);
    Wire.begin();

    display::begin();
    buzzer::begin();
    display::show(Status::Idle);

    bool rtc = clockw::begin();
    if (!rtc) Serial.println(F("FATAL: DS1302 missing, unset, or not answering - taps will be REFUSED"));

    store::begin();
    reader::begin();
    roster::begin();

    // Registered before modem::begin() runs so even boot-time blocking (the
    // AT ping retries in begin() itself, then syncClockFromNetwork() and
    // refreshRosterIfStale() below) stays tap-responsive — this is exactly
    // the "stuck at Idle for the whole boot, doesn't scan" window that
    // otherwise made the scanner look unresponsive before it ever reached
    // loop().
    modem::setIdleHook(idleDuringModemWait);

    bool modemOk = modem::begin();

    smsq::begin();
    relay::begin();

    if (modemOk) {
        uint32_t epoch;
        if (modem::syncClockFromNetwork(epoch)) { clockw::setFromEpoch(epoch); s_lastClock = millis(); }
        net::refreshRosterIfStale();
    }

    Serial.print(F("scanner-uno up | rtc=")); Serial.print(clockw::ok());
    Serial.print(F(" roster="));              Serial.print((unsigned)roster::size());
    Serial.print(F(" queued="));              Serial.print((unsigned)store::depth());
    Serial.print(F(" modem="));               Serial.println(modemOk ? F("ok") : F("FAILED"));
}

void loop() {
    uint32_t now = millis();

    buzzer::service();
    smsq::service();
    relay::service();

    // SMS_QUEUE_DEPTH is 1, so "back to empty" reliably means the one
    // offline-fallback job we just enqueued has been sent (or given up on,
    // if the modem itself failed) — replace "Buffering..." with a real
    // confirmation instead of leaving it on screen forever.
    if (s_awaitingSmsDone && smsq::depth() == 0) {
        display::show(Status::Ok, "SMS Sent");
        s_awaitingSmsDone = false;
    }

    // ------------------------------------------------------------ taps -----
    handleTap();

    // ----------------------------------------------------------- drain -----
    if (now - s_lastDrain >= DRAIN_INTERVAL_MS) {
        s_lastDrain = now;
        size_t sent = net::drain();
        if (sent) { Serial.print(F("delivered ")); Serial.println((unsigned)sent); }
    }

    // ---------------------------------------------------------- upkeep -----
    if (now - s_lastRoster >= ROSTER_CHECK_MS) {
        s_lastRoster = now;
        net::refreshRosterIfStale();
    }
    if (now - s_lastClock >= CLOCK_RESYNC_S * 1000UL) {
        s_lastClock = now;
        uint32_t epoch;
        if (modem::syncClockFromNetwork(epoch)) clockw::setFromEpoch(epoch);
    }
}
