#include <Arduino.h>
#include <Wire.h>
#include "reader.h"
#include "card.h"
#include "clock.h"
#include "display.h"
#include "buzzer.h"

// Card-only bench demo — same tap-handling shape as ../src/main.cpp's loop()
// but with modem/net/smsq/relay/roster/store all removed. Every tap shows
// the card's enrolled name on the LCD (falling back to the raw UID for a
// card with no offline payload written), exactly like the real firmware's
// display::show() call — just without anything touching the SIM800L.

void setup() {
    Serial.begin(9600);
    Wire.begin();

    display::begin();
    buzzer::begin();
    display::show(Status::Idle);

    bool rtc = clockw::begin();
    if (!rtc) Serial.println(F("WARNING: DS1302 missing/unset - timestamps will read 0"));

    reader::begin();

    Serial.println(F("card_demo up - tap a card"));
}

void loop() {
    buzzer::service();

    Tap t;
    if (reader::poll(t)) {
        CardData cd;
        bool hasCardData = card::read(cd);
        const char* label = hasCardData ? cd.name : t.uid;

        buzzer::play(Cue::Accepted);
        display::show(Status::Ok, label);

        Serial.print(F("tap uid=")); Serial.print(t.uid);
        if (hasCardData) {
            Serial.print(F(" phone=")); Serial.print(cd.phone);
            Serial.print(F(" id="));    Serial.print(cd.studentId);
            Serial.print(F(" name="));  Serial.println(cd.name);
        } else {
            Serial.println(F(" (no offline card data - showing raw UID)"));
        }

        reader::release();
    } else if (reader::sawDuplicate()) {
        buzzer::play(Cue::Duplicate);
    }
}
