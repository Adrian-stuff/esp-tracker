#include "feedback.h"
#include "../include/pins.h"
#include "../include/config.h"
#include <Arduino.h>

// RGB LED feedback (common anode, active LOW).
//
// Common anode → 3.3V. GPIO LOW = LED ON, GPIO HIGH = LED OFF.
// Color scheme:
//   Armed → orange (red + green)
//   Cancelled → red
//   Sent → red with faint blue
//   Acked → green (parent confirmed)
//   LowBattery → red tick

namespace feedback {

struct Step { uint16_t on_ms; uint16_t off_ms; };

// LED patterns — same visual signatures as before, now via RGB colors.
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

// Set RGB LED color. Common anode: 0 = full brightness, 255 = off.
static void setColor(uint8_t r, uint8_t g, uint8_t b) {
    analogWrite(PIN_LED_R, 255 - r);
    analogWrite(PIN_LED_G, 255 - g);
    analogWrite(PIN_LED_B, 255 - b);
}

// Drive the LED for the current cue — each cue gets its own color.
static void drive(bool on) {
    if (!on) {
        setColor(0, 0, 0);
        return;
    }

    switch (s_cue) {
        case Cue::Armed:
            setColor(255, 100, 0);   // orange — "SOS registered, hold to cancel"
            break;
        case Cue::Cancelled:
            setColor(255, 0, 0);     // red — "cancelled"
            break;
        case Cue::Sent:
            setColor(255, 0, 50);    // red with faint blue — "message sent"
            break;
        case Cue::Acked:
            setColor(0, 255, 0);     // green — "parent has been told"
            break;
        case Cue::LowBattery:
            setColor(255, 0, 0);     // red dim tick — "charge me"
            break;
        default:
            setColor(0, 0, 0);
            break;
    }
}

void begin() {
    // Configure RGB LED pins as outputs (LEDC PWM channels auto-assigned)
    pinMode(PIN_LED_R, OUTPUT);
    pinMode(PIN_LED_G, OUTPUT);
    pinMode(PIN_LED_B, OUTPUT);
    setColor(0, 0, 0);
}

void ledTest() {
    // Red for 1s
    setColor(255, 0, 0);
    delay(1000);
    // Green for 1s
    setColor(0, 255, 0);
    delay(1000);
    // Blue for 1s
    setColor(0, 0, 255);
    delay(1000);
    // Off
    setColor(0, 0, 0);
}

void clear() {
    s_cue = Cue::None;
    setColor(0, 0, 0);
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
                s_next = now + PATTERNS[(uint8_t)s_cue].steps[0].on_ms;
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
