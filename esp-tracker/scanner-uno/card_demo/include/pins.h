#pragma once
// Pin map — Arduino Uno + RC522 (SPI) + DS1302 (3-wire) + LCD (I2C) + SIM800L
// (SoftwareSerial).
//
// The original plan used a SIM900 GSM/GPRS shield here; after extensive
// testing (baud sweep across every common rate, both TX/RX orientations,
// both power configurations, confirmed ground) it never once responded to
// AT — while a SIM800L wired to the exact same pins responded immediately
// once a separate 5V power-rail glitch was fixed. Scrapped the SIM900 in
// favor of the SIM800L, which is proven working: confirmed AT -> OK at
// 9600 baud on real hardware, no PWRKEY handling needed (this breakout
// ties PWRKEY to GND itself, so it powers on as soon as VCC is applied).
//
// Unlike the ESP32 build, these are NOT remappable — SPI (11/12/13) and I2C
// (A4/A5) are fixed by the AVR, and there is exactly one hardware UART (0/1),
// which stays free for USB/debug. See PLAN.md §1b.

// ---- RC522 (hardware SPI) — 3.3V ONLY. 5V destroys the module. -----------
#define PIN_RFID_SCK   13
#define PIN_RFID_MISO  12
#define PIN_RFID_MOSI  11
#define PIN_RFID_SS    10
#define PIN_RFID_RST   9

// ---- I2C (hardware, A4/A5): LCD backpack (PCF8574) only -------------------
// The clock is NOT on this bus — see DS1302 below. This project's earlier
// plan assumed a DS3231 (I2C, 0x68); a live scan confirmed the actual RTC
// hardware on hand is a DS1302, which doesn't speak I2C at all.
#define PIN_I2C_SDA    A4
#define PIN_I2C_SCL    A5

// ---- DS1302 RTC — 3-wire (NOT I2C), own dedicated pins --------------------
// CE/IO/SCLK, not SDA/SCL. This is why it never showed up in any I2C scan —
// wrong protocol entirely, not a wiring fault.
#define PIN_RTC_CE     4
#define PIN_RTC_IO     3
#define PIN_RTC_SCLK   2

// ---- SIM800L (SoftwareSerial — the Uno has no spare hardware UART) -------
// Confirmed baud: 9600, verified against real hardware (AT -> OK).
//
// POWER — same warning as the tracker's SIM800L, and just as real here:
// up to 2A in transmit bursts. Feed it from its own buck converter set to
// 3.4-4.4V, NOT the Uno's 5V pin — a bare SIM800L (unlike the SIM900
// shield this replaced) has no onboard regulation of its own, and running
// it above ~4.4V risks damaging it, not just failing to respond. Put a
// bulk capacitor (1000-2200uF) physically close to the module — without
// one, the symptom is random resets that look exactly like a firmware bug.
//
// LEVEL SHIFT the Uno's TX line: SIM800L RX is not 5V tolerant. A simple
// 2-resistor divider on that one line is enough.
#define PIN_SIM_RX     7    // SIM800L TX -> Uno RX
#define PIN_SIM_TX     8    // Uno TX     -> SIM800L RX  *** VIA LEVEL SHIFTER/DIVIDER ***

// ---- Feedback --------------------------------------------------------------
#define PIN_BUZZER     6    // via transistor, never straight off the pin
