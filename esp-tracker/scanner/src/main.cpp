#include <Arduino.h>
#include "../include/pins.h"
#include "../include/config.h"
#include "reader.h"
#include "clock.h"
#include "roster.h"
#include "store.h"
#include "net.h"
#include "ui.h"
#include "smsq.h"
#include "notify.h"
#include "relay.h"

// Gate attendance station.
//
// Two rules shape this loop:
//
//   1. A TAP IS ACCEPTED EVEN WHEN THE NETWORK IS DOWN. The child's beep comes
//      from the local queue write, not a server round trip — otherwise a Wi-Fi
//      outage looks to a child like a rejected card, and they walk off assuming
//      they were not recorded.
//
//   2. NOTHING BLOCKS. A gate is a queue of thirty children; every millisecond
//      in delay() is one the reader is not accepting the next card.

static uint32_t s_lastDrain = 0;
static uint32_t s_lastRoster = 0;

void setup() {
    Serial.begin(115200);
    ui::begin();
    ui::play(Cue::Duplicate);              // one tick: "I am awake"

    bool rtc = clockw::begin();
    if (!rtc) {
        // Refuse to invent timestamps. A missing record is recoverable; a
        // record with a fabricated time silently corrupts the register.
        Serial.println(F("FATAL: DS3231 missing or lost power — taps will be REFUSED"));
    }

    store::begin();
    reader::begin();
    roster::begin();
    net::begin();
    smsq::begin();
    notify::begin();
    relay::begin();

    for (int i = 0; i < 40 && !net::online(); i++) delay(250);   // setup only
    if (net::online()) { clockw::syncFromNtp(); roster::refresh(); }

    Serial.printf("scanner up | rtc=%d roster=%u queued=%u sms=%s\n",
                  clockw::ok(), (unsigned)roster::size(), (unsigned)store::depth(),
                  !SIM900_PRESENT ? "not fitted (server will send)"
                                  : (smsq::ready() ? "ready" : "no signal"));
}

void loop() {
    uint32_t now = millis();

    net::service();
    ui::service();
    smsq::service();      // advances one AT step per loop; never blocks the reader
    relay::service();     // claims the server's outbound messages and acks them

    // ------------------------------------------------------------ taps -----
    Tap t;
    if (reader::poll(t)) {
        const bool known = roster::known(t.uid) || !roster::size();

        if (!clockw::ok()) {
            // See rule above: no clock, no record.
            ui::play(Cue::Error);
        } else {
            // Text FIRST, then store. The parent's message is the time-sensitive
            // half; the stored record is the durable half. Ordering it this way
            // means a full flash still gets the parent told.
            //
            // Direction is the server's call (it reorders a whole day, so late
            // offline taps self-correct). The device only knows whether this is
            // an odd or even tap today, so it says the neutral thing.
            if (known && !SMS_ON_UNKNOWN_CARD)
                t.sms_sent = notify::onTap(t, nullptr, "at the gate");
            else if (SMS_ON_UNKNOWN_CARD)
                t.sms_sent = notify::onTap(t, nullptr, "at the gate");

            if (!store::push(t))       ui::play(Cue::Error);      // flash full
            else if (!known)           ui::play(Cue::Unknown);    // still recorded
            else                       ui::play(net::online() ? Cue::Accepted : Cue::Offline);
        }
        Serial.printf("tap %s @%lu queued=%u sms=%d\n", t.uid,
                      (unsigned long)t.recorded_at, (unsigned)store::depth(), t.sms_sent);
    } else if (reader::sawDuplicate()) {
        ui::play(Cue::Duplicate);
    }

    // ----------------------------------------------------------- drain -----
    if (now - s_lastDrain >= DRAIN_INTERVAL_MS) {
        s_lastDrain = now;
        size_t sent = net::drain();
        if (sent) Serial.printf("delivered %u, %u left\n",
                                (unsigned)sent, (unsigned)store::depth());
    }

    // ---------------------------------------------------------- upkeep -----
    if (now - s_lastRoster >= ROSTER_CHECK_MS) {
        s_lastRoster = now;
        if (net::online()) {
            if (roster::stale()) roster::refresh();
            if (clockw::now() - clockw::lastSync() > NTP_RESYNC_S) clockw::syncFromNtp();
        }
    }

    // Online means EITHER uplink: Wi-Fi for the record, SIM900 for the text.
    // Showing red while SMS still works would misreport the gate as dead.
    ui::setHealth(net::online() || smsq::ready(), store::depth(), clockw::ok());
}
