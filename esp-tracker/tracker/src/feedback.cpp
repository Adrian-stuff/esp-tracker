#include "feedback.h"
#include "../include/pins.h"
#include "../include/config.h"
#include <Arduino.h>

namespace feedback {

struct Step { uint16_t on_ms; uint16_t off_ms; };

// Is this cue allowed to make noise? SOS cues are silent by default — see
// FEEDBACK_SILENT_SOS in config.h. Getting this wrong is not a cosmetic bug.
static bool audible(Cue c) {
    if (!FEEDBACK_USE_PIEZO) return false;
    if (!FEEDBACK_SILENT_SOS) return true;
    return !(c == Cue::Armed || c == Cue::Sent || c == Cue::Acked || c == Cue::Cancelled);
}

// Tone per cue, Hz. 0 = silent.
static uint16_t toneFor(Cue c) {
    switch (c) {
        case Cue::LowBattery: return 1200;
        default:              return 2400;
    }
}

// Patterns are chosen so a child can tell them apart without counting blinks:
// a flutter, a long steady glow, and a calm repeating heartbeat are three
// different things at a glance.
static const Step P_ARMED[]     = {{90,90},{90,90},{90,0}};        // quick flutter
static const Step P_CANCELLED[] = {{700,0}};                        // one long
static const Step P_SENT[]      = {{2000,0}};                       // steady glow
static const Step P_ACKED[]     = {{160,140},{160,900}};            // calm heartbeat
static const Step P_LOWBATT[]   = {{60,0}};                         // single tick

struct Pattern { const Step* steps; uint8_t n; uint32_t hold_ms; };

// hold_ms = 0 means "run the steps once". Non-zero repeats until elapsed.
static const Pattern PATTERNS[] = {
    {nullptr,     0, 0},
    {P_ARMED,     3, 0},
    {P_CANCELLED, 1, 0},
    {P_SENT,      1, 0},
    {P_ACKED,     2, FEEDBACK_ACK_HOLD_MS},
    {P_LOWBATT,   1, 0},
};

static Cue      s_cue     = Cue::None;
static uint8_t  s_step    = 0;
static bool     s_on      = false;
static uint32_t s_next    = 0;
static uint32_t s_started = 0;

static void drive(bool on) {
    // PIN_FEEDBACK is the LED now and the vibration motor later — same GPIO, so
    // the upgrade is a wiring change plus a config flag, not a code change.
    digitalWrite(PIN_FEEDBACK, on ? HIGH : LOW);

    if (!audible(s_cue)) { noTone(PIN_PIEZO); return; }
    if (on) tone(PIN_PIEZO, toneFor(s_cue));
    else    noTone(PIN_PIEZO);
}

void begin() {
    pinMode(PIN_FEEDBACK, OUTPUT);
    pinMode(PIN_PIEZO, OUTPUT);
    digitalWrite(PIN_FEEDBACK, LOW);
    noTone(PIN_PIEZO);
}

void clear() {
    s_cue = Cue::None;
    digitalWrite(PIN_FEEDBACK, LOW);
    noTone(PIN_PIEZO);
}

void play(Cue c) {
    // Pre-emption by priority: an ack must never be swallowed by a low-battery
    // tick that happened to be mid-blink.
    if (c <= s_cue && s_cue != Cue::None) return;
    s_cue     = c;
    s_step    = 0;
    s_on      = true;
    s_started = millis();
    s_next    = millis() + PATTERNS[(uint8_t)c].steps[0].on_ms;
    drive(true);
}

void service() {
    if (s_cue == Cue::None) return;
    uint32_t now = millis();
    if ((int32_t)(now - s_next) < 0) return;

    const Pattern& p = PATTERNS[(uint8_t)s_cue];

    if (s_on) {                                  // finished the ON phase
        s_on = false;
        drive(false);
        uint16_t off = p.steps[s_step].off_ms;
        if (off == 0 && s_step + 1 >= p.n) {     // pattern complete
            if (p.hold_ms && now - s_started < p.hold_ms) {
                s_step = 0; s_on = true; drive(true);
                s_next = now + p.steps[0].on_ms;
            } else {
                clear();
            }
            return;
        }
        s_next = now + off;
        return;
    }

    // finished the OFF phase — advance
    if (++s_step >= p.n) {
        if (p.hold_ms && now - s_started < p.hold_ms) s_step = 0;
        else { clear(); return; }
    }
    s_on = true;
    drive(true);
    s_next = now + p.steps[s_step].on_ms;
}

bool busy() { return s_cue != Cue::None; }

}
