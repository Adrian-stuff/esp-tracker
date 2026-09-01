#pragma once

// Runtime-configurable deployment settings, backed by NVS (ESP32 Preferences).
//
// config.h's DEFAULT_* values seed these on first boot, so a scanner that has
// never seen the config portal still comes up in a working (if generic)
// state instead of silently doing nothing. After that, whatever was saved
// through the portal wins — no reflash needed to point a scanner at a
// different server or a different family's phone.
//
// Values are cached in RAM after begin(): every HTTP request and every SMS
// reads through here, and that path must never block on flash.

namespace settings {
    void begin();     // loads from NVS, seeding DEFAULT_* on first boot

    const char* apiBase();
    const char* deviceToken();
    const char* smsPrimary();
    const char* smsSecondary();

    // Persists to NVS and updates the in-RAM cache. A reboot is still needed
    // for every subsystem to pick up the change — see net.cpp's saveCallback.
    void setApiBase(const char* v);
    void setDeviceToken(const char* v);
    void setSmsPrimary(const char* v);
    void setSmsSecondary(const char* v);
}
