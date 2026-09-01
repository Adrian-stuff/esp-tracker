#include <Arduino.h>
#include "../include/pins.h"
#include "../include/config.h"
#include "modem.h"
#include "gps.h"
#include "locator.h"
#include "sos.h"
#include "store.h"
#include "motion.h"
#include "feedback.h"
#include "notify.h"
#include "report.h"

// Tracker — ESP32 + SIM800L + NEO-6M + button.
//
// Build order note: this is Phase 01 from PLAN.md. The SOS path is the only
// thing that has to be right before anything else gets built on top of it.

// Battery estimation from uptime (no ADC hardware).
// Assumes a 2500mAh LiPo with average ~5mA draw (CSCLK sleep + periodic scans).
// This is a rough estimate — real battery life depends heavily on usage patterns.
// Reset the counter when the device is known to be freshly charged.
static uint32_t s_bootMs = 0;
static constexpr float BATTERY_CAPACITY_MAH = 2500.0f;
static constexpr float AVG_DRAW_MA = 5.0f;      // conservative estimate
static constexpr float BATTERY_WARN_PCT = 20.0f;
static constexpr float BATTERY_CRIT_PCT = 10.0f;
static bool s_batteryWarned = false;
static bool s_batteryCrit = false;

static uint8_t estimateBatteryPct() {
    uint32_t hoursUp = (millis() - s_bootMs) / 3600000UL;
    float used_mAh = hoursUp * AVG_DRAW_MA;
    float pct = 100.0f * (1.0f - used_mAh / BATTERY_CAPACITY_MAH);
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return (uint8_t)pct;
}

static void checkBattery() {
    uint8_t pct = estimateBatteryPct();
    if (pct <= BATTERY_CRIT_PCT && !s_batteryCrit) {
        s_batteryCrit = true;
        notify::fire(Event::BatteryCritical, nullptr);
        feedback::play(Cue::LowBattery);
    } else if (pct <= BATTERY_WARN_PCT && !s_batteryWarned) {
        s_batteryWarned = true;
        notify::fire(Event::BatteryLow, nullptr);
        feedback::play(Cue::LowBattery);
    }
}

static uint32_t s_buttonDownAt = 0;

static void serviceButton() {
    bool down = (digitalRead(PIN_SOS_BUTTON) == LOW);

    if (down && s_buttonDownAt == 0) {
        s_buttonDownAt = millis();
    } else if (!down) {
        s_buttonDownAt = 0;
    } else if (sos::active() && millis() - s_buttonDownAt >= SOS_HOLD_MS) {
        // Second 2s hold during the cancel window aborts an accidental press.
        sos::cancel();
        s_buttonDownAt = 0;
    } else if (!sos::active() && millis() - s_buttonDownAt >= SOS_HOLD_MS) {
        // First 2s hold, never a tap: pocket false alarms train parents to ignore it.
        sos::trigger();
        s_buttonDownAt = 0;
    }
}

void setup() {
    Serial.begin(115200);
    s_bootMs = millis();

    store::begin();
    sos::begin();
    gps::begin();
    locator::begin();
    motion::begin();
    notify::begin();
    modem::begin();
    report::begin();

    // AT+CCLK? works over plain network registration — no GPRS attach
    // needed (see modem.cpp's file header for why that path is dropped).
    // The ESP32 has no RTC of its own, so this is the only clock source;
    // report.cpp's recorded_at and sos.cpp's timestamps are wrong without it.
    modem::syncClockFromNetwork();
}

// Periodic battery check interval (every 30 minutes)
static uint32_t s_lastBatteryCheck = 0;
static constexpr uint32_t BATTERY_CHECK_INTERVAL_MS = 30UL * 60UL * 1000UL;

void loop() {
    uint32_t now = millis();

    serviceButton();
    sos::service();
    locator::service();
    motion::service();
    report::service();
    store::drain();
    modem::closeIdle();

    // Periodic battery check (estimated, no ADC)
    if (now - s_lastBatteryCheck >= BATTERY_CHECK_INTERVAL_MS) {
        s_lastBatteryCheck = now;
        checkBattery();
    }
}
