#include "buzzer.h"
#include "../include/pins.h"
#include <Arduino.h>

static uint32_t s_offAt = 0;

namespace buzzer {

void begin() { pinMode(PIN_BUZZER, OUTPUT); noTone(PIN_BUZZER); }

void play(Cue c) {
    uint16_t freq, ms;
    switch (c) {
        case Cue::Accepted:  freq = 2000; ms =  80; break;
        case Cue::Offline:   freq = 1200; ms = 150; break;
        case Cue::Unknown:   freq =  600; ms = 300; break;
        case Cue::Duplicate: freq = 2500; ms =  40; break;
        case Cue::Error:     freq =  300; ms = 400; break;
    }
    // Passing a duration lets the AVR core's own timer ISR stop the tone,
    // independent of loop() ever calling service() again in time. That
    // matters here specifically: modem::sendSms() (see smsq.cpp) blocks
    // the whole loop() for the AT+CMGS exchange, up to SMS_SEND_TIMEOUT_MS
    // — with the old 2-arg tone() (which never stops on its own), any cue
    // played just before that blocking call kept sounding for the entire
    // send, which read as one continuous beep with no way to scan another
    // card until it finished. s_offAt/service() stay as a defensive
    // backstop, not the primary stop mechanism anymore.
    tone(PIN_BUZZER, freq, ms);
    s_offAt = millis() + ms;
}

void service() {
    if (s_offAt && (int32_t)(millis() - s_offAt) >= 0) {
        noTone(PIN_BUZZER);
        s_offAt = 0;
    }
}

}
