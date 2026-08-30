#pragma once

// Piezo buzzer, plain GPIO via transistor. No PWM library needed — tone()
// is built into the AVR core and is non-blocking once started.

enum class Cue { Accepted, Offline, Unknown, Duplicate, Error };

namespace buzzer {
    void begin();
    void play(Cue c);   // fire-and-forget; does not block the loop
    void service();     // stops the tone once its duration has elapsed
}
