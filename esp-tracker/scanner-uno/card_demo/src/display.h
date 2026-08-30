#pragma once

// Feedback display — a character LCD over I2C (PCF8574 backpack), sharing
// the bus with the DS3231 (A4/A5). Replaces the RGB-LED-over-I2C design:
// the module actually on hand is an LCD backpack, not a PCA9633 — see
// config.h for how that was discovered. Status is a short line of text
// instead of a colour, which is arguably better feedback anyway (a child
// can read "Tap OK" — no colour-blindness ambiguity, no memorizing what
// blue vs orange means).

enum class Status { Ok, Error, Buffering, Unknown, Idle };

namespace display {
    void begin();
    // detail is optional second-line text (e.g. a queue depth, a name) —
    // pass nullptr to leave the second line as-is.
    void show(Status s, const char* detail = nullptr);
}
