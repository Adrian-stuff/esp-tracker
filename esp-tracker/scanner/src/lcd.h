#pragma once

// I2C character LCD (PCF8574 backpack), same as the Uno build.
// Shares the I2C bus with the DS3231 at addresses 0x27 / 0x68.

namespace lcd {
    void begin();
    void show(const char* line1, const char* line2 = nullptr);
    void clear();
}
