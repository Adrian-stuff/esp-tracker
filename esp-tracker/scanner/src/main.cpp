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
#include "sms.h"
#include "notify.h"
#include "relay.h"
#include "card.h"
#include "lcd.h"

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
    lcd::begin();
    ui::begin();
    ui::play(Cue::Duplicate);              // one tick: "I am awake"

    bool rtc = clockw::begin();
    if (!rtc) {
        // Refuse to invent timestamps. A missing record is recoverable; a
        // record with a fabricated time silently corrupts the register.
        Serial.println(F("FATAL: DS3231 missing or lost power — taps will be REFUSED"));
        lcd::show("ERROR", "No RTC");
    }

    store::begin();
    reader::begin();
    roster::begin();
    net::begin();       // WiFiManager: connects or starts config portal
    smsq::begin();
    sms::begin();
    notify::begin();
    relay::begin();

    // net::begin() blocks until connected or portal times out.
    // If the portal was active and timed out, the ESP32 rebooted inside net::begin().
    if (net::online()) { clockw::syncFromNtp(); roster::refresh(); }

    Serial.printf("scanner up | rtc=%d roster=%u queued=%u sms=%s\n",
                  clockw::ok(), (unsigned)roster::size(), (unsigned)store::depth(),
                  !SIM900_PRESENT ? "not fitted (server will send)"
                                  : (smsq::ready() ? "ready" : "no signal"));

    if (net::online())
        lcd::show("Online", "Loading...");
    else
        lcd::show("Offline", "Loading...");
}

void loop() {
    uint32_t now = millis();

    net::service();
    ui::service();
    smsq::service();      // advances one AT step per loop; never blocks the reader
    sms::pollInbox();     // polls for tracker SMS, relays to server via WiFi
    relay::service();     // claims the server's outbound messages and acks them

    // ------------------------------------------------------------ taps -----
    Tap t;
    if (reader::poll(t)) {
        const bool known = roster::known(t.uid) || !roster::size();

        if (!clockw::ok()) {
            // See rule above: no clock, no record.
            ui::play(Cue::Error);
            lcd::show("ERROR", "No clock");
        } else {
            // Offline fallback: read card data for direct SMS when WiFi is down.
            // The card is still SELECTED by reader::poll(), so we can read the
            // sector before halting it.
            CardData cardData;
            bool hasCardData = false;

            if (!store::push(t)) {
                ui::play(Cue::Error);      // flash full
                lcd::show("ERROR", "Queue full");
            } else if (known && !SMS_ON_UNKNOWN_CARD) {
                t.sms_sent = notify::onTap(t, nullptr, "at the gate");
                ui::play(net::online() ? Cue::Accepted : Cue::Offline);

                // Try to read card data for LCD display
                hasCardData = card::read(cardData);
                if (hasCardData) {
                    lcd::show(cardData.name, "Tap recorded");
                } else {
                    lcd::show("Welcome", t.uid);
                }

                // Offline fallback: if WiFi is down and we have card data,
                // send a direct SMS using the phone number from the card.
                if (!net::online() && hasCardData && SIM900_PRESENT && smsq::ready()) {
                    uint32_t local = t.recorded_at + (uint32_t)(TZ_OFFSET_S);
                    int hh = (local % 86400) / 3600;
                    int mm = (local % 3600) / 60;
                    char body[152];
                    snprintf(body, sizeof body, "%s tapped in at %02d:%02d.",
                             cardData.name, hh, mm);
                    smsq::enqueue(cardData.phone, body);
                    t.sms_sent = true;
                }
            } else if (SMS_ON_UNKNOWN_CARD) {
                t.sms_sent = notify::onTap(t, nullptr, "at the gate");
                ui::play(net::online() ? Cue::Accepted : Cue::Offline);
                lcd::show("Unknown card", t.uid);
            } else {
                // Unknown card, not in direct SMS mode
                ui::play(net::online() ? Cue::Unknown : Cue::Offline);
                lcd::show("Unknown card", t.uid);
            }

            // Release the card after we're done reading
            reader::release();
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

    // Update LCD with ambient status when not showing a tap
    static uint32_t s_lastLcd = 0;
    if (now - s_lastLcd > 5000) {
        s_lastLcd = now;
        if (net::online() && store::depth() == 0) {
            lcd::show("Online", "Ready to scan");
        } else if (!net::online() && store::depth() > 0) {
            char detail[17];
            snprintf(detail, sizeof detail, "Queued: %u", (unsigned)store::depth());
            lcd::show("Offline", detail);
        }
    }
}
