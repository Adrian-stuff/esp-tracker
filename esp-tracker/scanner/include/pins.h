#pragma once
// Pin map — ESP32 DevKit v1 + RC522 (SPI) + DS1302 RTC (3-wire) + SIM900 (UART2).

// ---- RC522 (SPI) — 3.3V ONLY. 5V destroys the module. --------------------
#define PIN_RFID_SS    5
#define PIN_RFID_SCK   18
#define PIN_RFID_MOSI  23
#define PIN_RFID_MISO  19
#define PIN_RFID_RST   4

// ---- DS1302 RTC (3-wire, NOT I2C) ----------------------------------------
#define PIN_RTC_CLK    14
#define PIN_RTC_DAT    15
#define PIN_RTC_CE     2

// ---- I2C: LCD only (PCF8574 backpack) ------------------------------------
#define PIN_I2C_SDA    21
#define PIN_I2C_SCL    22

// ---- SIM900 (UART2) -------------------------------------------------------
// POWER: same rule as the tracker's SIM800L — up to 2 A in transmit bursts at
// 3.4-4.4 V. It CANNOT run from the ESP32's 3V3 pin or straight off USB 5 V.
// Use a buck regulator to ~4 V (or a shield with its own) plus a 1000-2200 uF
// bulk cap close to the module. Mains powered here, so no battery budget to
// worry about — but the current spike is identical.
//
// PWRKEY is a TOGGLE — each ~1.2 s LOW pulse flips ON↔OFF. GPIO32 floats
// during ESP32 boot, randomly toggling the module. Fix: set HIGH immediately
// in begin() to pin the line. See smsq.cpp for the full boot sequence.
#define PIN_SIM_RX     16   // SIM900 TX -> ESP32 RX
#define PIN_SIM_TX     17   // ESP32 TX  -> SIM900 RX
#define PIN_SIM_PWRKEY 32   // hold LOW ~1.2 s to toggle module power

// ---- Feedback -------------------------------------------------------------
#define PIN_BUZZER     25   // via transistor, never straight off the pin

// Two discrete LEDs, or the R and G legs of one 4-pin RGB LED — the mapping is
// the same either way, which is why adding RGB needed no rewiring of these two.
#define PIN_LED_OK     26   // green leg
#define PIN_LED_ERR    27   // red leg
#define PIN_LED_B      13   // blue leg; ignored unless LED_IS_RGB. GPIO 13 is
                            // free of strapping duties, unlike 12.
