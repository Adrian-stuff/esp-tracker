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

// Tracker — ESP32 + SIM800L + NEO-6M + button.
//
// Build order note: this is Phase 01 from PLAN.md. The SOS path is the only
// thing that has to be right before anything else gets built on top of it.

static uint32_t s_buttonDownAt = 0;

static void serviceButton() {
    bool down = (digitalRead(PIN_SOS_BUTTON) == LOW);

    if (down && s_buttonDownAt == 0) {
        s_buttonDownAt = millis();
    } else if (!down) {
        s_buttonDownAt = 0;
    } else if (!sos::active() && millis() - s_buttonDownAt >= SOS_HOLD_MS) {
        // 2s hold, never a tap: pocket false alarms train parents to ignore it.
        sos::trigger();
        s_buttonDownAt = 0;
    }
}

void setup() {
    Serial.begin(115200);

    store::begin();
    sos::begin();
    gps::begin();
    locator::begin();
    motion::begin();
    notify::begin();
    modem::begin();

    // Clock first: TLS validation needs a roughly-correct time and the ESP32
    // has no RTC. Without this every handshake fails for no obvious reason.
    modem::attach();
    modem::syncClockFromNetwork();
}

void loop() {
    serviceButton();
    sos::service();
    locator::service();
    motion::service();
    modem::closeIdle();

    // TODO: adaptive cadence (PLAN.md 4.1), driven by motion::state():
    //   Stationary -> 1 report / 30 min
    //   Moving     -> 1 report / 2 min
    //   blind()    -> no usable signal (rural, no APs): fall back to a fixed
    //                 10 min cadence rather than trusting "stationary".
    // TODO: drain store:: queue in BATCHES (one handshake, many events),
    //       releasing entries only on HTTP 200.
    // TODO: battery <=20% / <=10% -> feedback::play(Cue::LowBattery)
    //       + notify::fire(Event::BatteryLow / BatteryCritical, nullptr).
    // TODO: on-device geofence test -> notify::fire(Event::GeofenceExit, "school").
    // TODO: battery sense on ADC1 (GPIO34); alert at 20% AND 10%.
}
