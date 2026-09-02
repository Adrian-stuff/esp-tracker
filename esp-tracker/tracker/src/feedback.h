#pragma once
#include <stdint.h>

// What the child perceives. Hardware sits behind this on purpose: the vibration
// motor is out of stock, so an LED stands in, and swapping back is one flag in
// config.h rather than an edit through the SOS path.
//
// The substitution is not neutral, and the patterns below are shaped around it:
//
//   A buzz is FELT. It reaches a child whose device is in a pocket or a
//   backpack, who is frightened, and who is not looking at anything.
//   An LED has to be LOOKED AT.
//
// Two consequences, both handled here:
//
//   1. ACK PERSISTS. With haptics, "your parent has been told" is a one-shot
//      because the child feels it the moment it happens. An LED they are not
//      looking at is a cue that never arrives, so the ack repeats for
//      FEEDBACK_ACK_HOLD_MS to give them time to look down.
//   2. NOTHING BLOCKS. Holding the SOS path in delay() for 20 s would be
//      absurd, so every cue is millis()-driven and service() is called from
//      loop(). This also removes the 300 ms delay that used to sit inside
//      sos::trigger(), on the path racing a 5-second deadline.
//
// A piezo is fitted alongside the LED, and carries every cue EXCEPT the SOS
// ones. An SOS is the one case where being heard can be the danger: the child
// presses the button precisely when someone is frightening them, and a device
// that beeps announces both the child and the fact that they called for help.
// SOS cues therefore stay silent and visual (FEEDBACK_SILENT_SOS in config.h).
//
// Which is the real argument for the vibration motor: haptics are the only
// channel that is both silent AND felt.

// Order IS priority: play() below rejects a new cue whose enum value is <=
// the currently-playing one, so higher-numbered cues pre-empt lower ones.
// LowBattery is an ambient background tick and must never interrupt an
// active SOS cue, so it stays numerically LOWEST (right after None) even
// though it is declared last for readability — a low-battery check runs
// unconditionally every 30 min in main.cpp regardless of SOS state, so this
// ordering is load-bearing, not cosmetic. (Previously LowBattery was last in
// BOTH declaration order and value, making it the highest-priority cue and
// letting a routine battery tick silently clobber Armed/Sent/Acked mid-SOS —
// exactly backwards from this comment's own stated intent.)
enum class Cue : uint8_t {
    None = 0,
    LowBattery,   // ambient, lowest priority — see note above
    Armed,        // 2 s hold completed — "it registered"
    Cancelled,    // aborted inside the cancel window
    Sent,         // transmitted at t+5 s
    Acked,        // server confirmed — "your parent has been told"
};

namespace feedback {
    void begin();
    void ledTest();             // flash R, G, B at boot to verify wiring
    void service();          // non-blocking; call every loop()
    void play(Cue c);        // higher-priority cues pre-empt lower ones
    void clear();
    bool busy();
}
