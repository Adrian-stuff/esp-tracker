#pragma once
#include <stdint.h>

// WiFi-direct uplink — PRESENTATION/DEMO BACKUP PATH.
//
// Two independent jobs, both gated on being connected to WiFi:
//
//   1. PUSH this tracker's own location straight to Supabase's `ingest`
//      function, bypassing the SIM800L -> scanner SMS relay chain
//      entirely. Runs ALONGSIDE report.cpp's existing SMS reporting, not
//      instead of it — either path reaching the server is enough, and
//      this one is faster/more reliable whenever real WiFi is available
//      (e.g. at a presentation venue), at the cost of needing that WiFi.
//
//   2. PULL any outbox message addressed to this tracker's own child —
//      e.g. an attendance-tap notification — and deliver it via THIS
//      device's own SIM800L SMS. supabase/migrations/0005_outbox.sql's own
//      comment already anticipated a tracker doing exactly this ("a
//      tracker may only relay messages about its own child"); the
//      server-side filtering for it already existed in
//      supabase/functions/outbox — nothing there needed to change, only a
//      client that actually claims and relays. This exists specifically
//      because the SCANNER's SIM900A is the flaky link for tap
//      notifications: when it's down, this tracker can still deliver the
//      same notification through its own, independent SIM800L.
//
// Uses the ESP32's own WiFi + WiFiClientSecure + HTTPClient — entirely
// separate hardware from the SIM800L modem, so nothing here contends with
// modem.cpp's AT-command traffic. It DOES share the WiFi radio with
// motion.cpp's periodic scan-for-motion-detection: scanning while
// associated to an AP is supported on ESP32 but can briefly interrupt the
// connection. Acceptable for a presentation; not solved here.
namespace wifi_uplink {
    void begin();     // loads SSID/password from Preferences (config.h defaults)
    void service();   // called every loop() — connects, then does both jobs on their own timers
    bool connected();
    void off();       // disconnect + WiFi radio off — frees ~70-135mA before SMS TX
    void restore();   // re-enable WiFi STA after SMS — motion detection needs it
}
