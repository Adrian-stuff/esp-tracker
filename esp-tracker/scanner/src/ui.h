#pragma once
#include <stdint.h>

// Gate feedback — non-blocking, for the same reason the tracker's is.
//
// A gate is a QUEUE. Thirty children arrive at once, and every millisecond the
// loop spends in delay() is a millisecond the reader is not accepting the next
// card. The scaffold's delay(FEEDBACK_HOLD_MS) after each tap put a hard
// ~1.3 s floor on throughput and made the reader feel broken under load.
//
// Cues are deliberately distinguishable by EAR, not by light: children at a
// gate are looking at each other, not at a box on a post.

enum class Cue : uint8_t {
    None = 0,
    Accepted,     // known card, queued          — short rising chirp, green
    Unknown,      // card not on the roster      — low double buzz, red
    Duplicate,    // same card inside the debounce window — soft tick, amber
    Offline,      // queued but network is down  — accepted chirp + amber
    Error         // could not queue: flash full — long low tone, red
};

namespace ui {
    void begin();
    void service();          // call every loop()
    void play(Cue c);
    bool busy();

    // Ambient state shown between taps: solid green = online and synced,
    // slow amber = buffering offline, fast red = something is wrong.
    void setHealth(bool online, uint16_t queueDepth, bool rtcOk);
}
