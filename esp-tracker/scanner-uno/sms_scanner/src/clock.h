#pragma once
#include <stdint.h>

// DS1302 — 3-wire (CE/IO/SCLK), NOT I2C. This project's earlier plan
// assumed a DS3231; a live I2C bus scan confirmed the actual RTC hardware
// on hand is a DS1302, which explains why it never showed up on the bus —
// wrong protocol, not a wiring fault. See pins.h.
//
// *** A WEAKER GUARANTEE THAN THE DS3231 DESIGN ELSEWHERE IN THIS PROJECT ***
// DS3231 has a hardware oscillator-stop flag (OSF) that's automatically set
// the instant backup power is lost, giving a reliable "distrust this time"
// signal. DS1302 has no equivalent: its "clock halt" bit is purely
// software-controlled, not automatically set on power loss. ok() here can
// only catch a DS1302 reporting outright garbage (register values out of
// range) or one that was never started — NOT a DS1302 that silently lost
// backup power and is now confidently reporting a plausible-but-wrong time.
// If the backup cell/supercap is missing or dead, this will NOT catch it.
//
// Same refusal rule as the ESP32 build otherwise: if the RTC reports
// nothing trustworthy, the scanner REFUSES to queue offline taps rather
// than recording them with a time it made up.
//
// This module does NOT reach for the network itself — there is no NTP
// here. main.cpp asks modem::syncClockFromNetwork() for NITZ time
// (AT+CCLK?) and feeds the result in via setFromEpoch(), which keeps this
// module ignorant of the modem and the modem's single shared AT-command
// channel.

namespace clockw {
    bool begin();
    bool ok();                       // RTC present and not flagged as having lost power
    uint32_t now();                  // unix seconds, 0 if !ok()
    void setFromEpoch(uint32_t epoch);
    uint32_t lastSync();
}
