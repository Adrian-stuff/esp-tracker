#pragma once
#include <stdint.h>

// Battery percentage — see config.h's BATTERY_HAS_ADC_SENSOR.
//
// Current hardware has NO battery sensor fitted (confirmed — pins.h's
// PIN_VBAT_SENSE is a reservation for a future board, not a populated
// divider). pct() therefore returns a rough uptime-based estimate by
// default: honest about being an estimate, not dressed up as a real
// reading. If a future board adds the real ADC + divider, flip
// BATTERY_HAS_ADC_SENSOR on and pct() switches to reading it — no callers
// need to change.

namespace battery {
    void begin();   // records boot time for the uptime-estimate fallback

    // 0-100. Real ADC reading if BATTERY_HAS_ADC_SENSOR, else an uptime-based
    // estimate — see config.h and battery.cpp for which and why.
    uint8_t pct();

    // Raw millivolts at the ESP32 pin, pre-divider. Only meaningful when
    // BATTERY_HAS_ADC_SENSOR is true; returns 0 otherwise (diagnostics only,
    // e.g. the BLE STATUS command — not used to compute pct() either way).
    uint32_t milliVolts();
}
