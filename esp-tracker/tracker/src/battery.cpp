#include "battery.h"
#include "../include/pins.h"
#include "../include/config.h"
#include <Arduino.h>

// LiPo voltage curve endpoints (millivolts, AT THE CELL — after undoing the
// divider). Only used if BATTERY_HAS_ADC_SENSOR is ever turned on for a
// board that actually has the divider. Most single-cell LiPo protection
// circuits cut off around 3.0-3.2V; treating 3.3V as "0%" avoids reporting a
// confident-looking percentage in the steep, non-linear tail of the curve.
static constexpr float LIPO_EMPTY_MV = 3300.0f;
static constexpr float LIPO_FULL_MV  = 4200.0f;
static constexpr uint8_t ADC_SAMPLES = 8;

static uint32_t s_bootMs = 0;

namespace battery {

void begin() {
    s_bootMs = millis();
    if (BATTERY_HAS_ADC_SENSOR) {
        analogSetPinAttenuation(PIN_VBAT_SENSE, ADC_11db);
    }
}

uint32_t milliVolts() {
    if (!BATTERY_HAS_ADC_SENSOR) return 0;   // no sensor fitted — see config.h
    uint32_t sum = 0;
    for (uint8_t i = 0; i < ADC_SAMPLES; i++) sum += analogReadMilliVolts(PIN_VBAT_SENSE);
    return sum / ADC_SAMPLES;
}

static uint8_t estimateFromUptime() {
    uint32_t hoursUp = (millis() - s_bootMs) / 3600000UL;
    float used_mAh = hoursUp * BATTERY_AVG_DRAW_MA;
    float p = 100.0f * (1.0f - used_mAh / BATTERY_CAPACITY_MAH);
    if (p < 0.0f)   p = 0.0f;
    if (p > 100.0f) p = 100.0f;
    return (uint8_t)p;
}

uint8_t pct() {
    if (!BATTERY_HAS_ADC_SENSOR) return estimateFromUptime();

    float cellMv = (float)milliVolts() * VBAT_DIVIDER_RATIO;
    float p = (cellMv - LIPO_EMPTY_MV) / (LIPO_FULL_MV - LIPO_EMPTY_MV) * 100.0f;
    if (p < 0.0f)   p = 0.0f;
    if (p > 100.0f) p = 100.0f;
    return (uint8_t)(p + 0.5f);
}

}
