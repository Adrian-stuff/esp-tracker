#include "ui.h"
#include "../include/pins.h"
#include "../include/config.h"
#include <Arduino.h>

namespace ui {

struct Note { uint16_t hz; uint16_t ms; };

// Rising = good, falling = not. Consistent across every cue so it reads
// correctly the first time a child hears it.
static const Note N_ACCEPTED[]  = {{1800,70},{2400,90}};
static const Note N_UNKNOWN[]   = {{600,140},{0,60},{600,140}};
static const Note N_DUPLICATE[] = {{1600,45}};
static const Note N_OFFLINE[]   = {{1800,70},{2100,70},{0,40},{1400,90}};
static const Note N_ERROR[]     = {{400,420}};

struct Pattern { const Note* n; uint8_t len; bool r; bool g; bool b; };
// R, G, B per cue. With two discrete LEDs the blue column is simply ignored;
// with an RGB LED it gives "stored, not yet sent" its own colour instead of
// making red mean two different things.
static const Pattern PATTERNS[] = {
    {nullptr,     0, false, false, false},
    {N_ACCEPTED,  2, false, true,  false},   // green  — enrolled, queued
    {N_UNKNOWN,   3, true,  true,  false},   // amber  — not on the roster
    {N_DUPLICATE, 1, false, false, false},
    {N_OFFLINE,   4, false, false, true },   // blue   — stored, network down
    {N_ERROR,     1, true,  false, false},   // red    — could not store
};

// One place that knows the polarity. A COMMON ANODE LED lights when the pin is
// pulled LOW, so every write has to flip — doing it here rather than at each
// call site is what keeps that from being a bug hunt later.
static void led(uint8_t pin, bool on) {
    digitalWrite(pin, (on != LED_COMMON_ANODE) ? HIGH : LOW);
}

static Cue      s_cue   = Cue::None;
static uint8_t  s_i     = 0;
static uint32_t s_next  = 0;
static bool     s_online = true;
static uint16_t s_depth  = 0;
static bool     s_rtcOk  = true;
static uint32_t s_ambient = 0;

static void allOff() {
    noTone(PIN_BUZZER);
    led(PIN_LED_OK, false);
    led(PIN_LED_ERR, false);
    if (LED_IS_RGB) led(PIN_LED_B, false);
}

void begin() {
    pinMode(PIN_LED_OK, OUTPUT);
    pinMode(PIN_LED_ERR, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);
    if (LED_IS_RGB) pinMode(PIN_LED_B, OUTPUT);
    allOff();
}

void play(Cue c) {
    s_cue = c; s_i = 0; s_next = 0;
    const Pattern& p = PATTERNS[(uint8_t)c];
    led(PIN_LED_ERR, p.r);
    led(PIN_LED_OK,  p.g);
    if (LED_IS_RGB) led(PIN_LED_B, p.b);
}

bool busy() { return s_cue != Cue::None; }

void setHealth(bool online, uint16_t queueDepth, bool rtcOk) {
    s_online = online; s_depth = queueDepth; s_rtcOk = rtcOk;
}

void service() {
    uint32_t now = millis();

    if (s_cue != Cue::None) {
        const Pattern& p = PATTERNS[(uint8_t)s_cue];
        if (now >= s_next) {
            if (s_i >= p.len) {                      // pattern finished
                s_cue = Cue::None;
                allOff();
            } else {
                const Note& n = p.n[s_i++];
                if (n.hz) tone(PIN_BUZZER, n.hz); else noTone(PIN_BUZZER);
                s_next = now + n.ms;
            }
        }
        return;                                       // cues own the LEDs
    }

    // Ambient health, between taps. A gate scanner sits unattended for hours;
    // it must be obvious from across the corridor that it is still working.
    if (now - s_ambient < 1000) return;
    s_ambient = now;
    static bool blink = false;
    blink = !blink;
    if (!s_rtcOk) {                                   // worst state: wrong time
        led(PIN_LED_ERR, blink); led(PIN_LED_OK, false);
        if (LED_IS_RGB) led(PIN_LED_B, false);
    } else if (s_online && s_depth == 0) {            // healthy
        led(PIN_LED_OK, true); led(PIN_LED_ERR, false);
        if (LED_IS_RGB) led(PIN_LED_B, false);
    } else if (LED_IS_RGB) {                          // buffering: its own colour
        led(PIN_LED_B, blink); led(PIN_LED_OK, false); led(PIN_LED_ERR, false);
    } else {                                          // two-LED fallback
        led(PIN_LED_OK, false); led(PIN_LED_ERR, blink);
    }
}

}
