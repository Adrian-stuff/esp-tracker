// I2C-only isolation test — DS3231 clock + PCF8574 LCD backpack, sharing
// the bus (SDA=A4, SCL=A5). Nothing else loaded, so this has RAM headroom
// the combined selftest didn't — see platformio.ini.
//
// The "LED" in this project's earlier plan turned out to be an LCD
// backpack (confirmed by a live scan finding it at 0x27, not the 0x62
// assumed for a PCA9633 RGB driver) — its 8 I2C-expander pins are
// hard-wired to LCD control signals, not free GPIO. This test drives it as
// what it actually is.
//
// Commands: r = rerun scan+DS3231   l = write a test pattern to the LCD

#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>
#include <LiquidCrystal_I2C.h>
#include <avr/wdt.h>

#define LCD_I2C_ADDR 0x27   // must match scanner-uno/include/config.h
#define LCD_COLS     16
#define LCD_ROWS     2

RTC_DS3231 rtc;
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);

static void rule() { Serial.println(F("--------------------------------------------------")); }

static void testI2C() {
    rule();
    Serial.println(F("I2C SCAN"));
    rule();
    Wire.begin();
    Wire.setWireTimeout(25000, true);   // a stuck bus reports cleanly instead of hanging forever

    int found = 0;
    bool sawLcd = false, sawRtc = false;
    for (uint8_t a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            Serial.print(F("  device at 0x"));
            if (a < 16) Serial.print('0');
            Serial.print(a, HEX);
            if (a == 0x68) { Serial.print(F("  <- DS3231")); sawRtc = true; }
            if (a == LCD_I2C_ADDR) { Serial.print(F("  <- LCD backpack (PCF8574)")); sawLcd = true; }
            if (a == 0x57) Serial.print(F("  <- likely AT24C32 eeprom on the DS3231 breakout"));
            Serial.println();
            found++;
        }
    }
    if (!found) {
        if (Wire.getWireTimeoutFlag()) {
            Serial.println(F("  BUS STUCK - timed out. Check SDA/SCL aren't swapped or shorted"));
            Wire.clearWireTimeoutFlag();
        } else {
            Serial.println(F("  nothing responded - check SDA=A4, SCL=A5, power, GND"));
        }
        return;
    }
    if (!sawRtc) Serial.println(F("  NOTE: no device at 0x68 - DS3231 not detected"));
    if (!sawLcd) Serial.println(F("  NOTE: no device at 0x27 - LCD backpack not detected"));

    rule();
    Serial.println(F("DS3231"));
    rule();
    if (!rtc.begin()) {
        Serial.println(F("  on the bus but not answering as a DS3231"));
    } else if (rtc.lostPower()) {
        Serial.println(F("  LOST POWER - fit/replace the CR2032, then set the time"));
    } else {
        DateTime n = rtc.now();
        float t = rtc.getTemperature();
        int whole = (int)t, tenths = (int)((t - whole) * 10);
        if (tenths < 0) tenths = -tenths;
        char buf[48];
        snprintf(buf, sizeof buf, "  %04d-%02d-%02d %02d:%02d:%02d  (%d.%d C)",
                 n.year(), n.month(), n.day(), n.hour(), n.minute(), n.second(), whole, tenths);
        Serial.println(buf);
    }
}

static void testLcd() {
    rule();
    Serial.println(F("LCD test pattern"));
    rule();
    lcd.init();
    lcd.backlight();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("LCD OK  16x2?");
    lcd.setCursor(0, 1);
    lcd.print("If you see this");
    Serial.println(F("  wrote a test pattern - check the physical screen"));
    Serial.println(F("  if nothing shows: try the contrast trimmer potentiometer on the backpack"));
}

void setup() {
    wdt_disable();
    Serial.begin(115200);
    delay(600);
    Serial.println(F("\n=== I2C-ONLY isolation test (DS3231 + PCF8574 LCD backpack) ==="));
    testI2C();
    Serial.println(F("\nCommands: r = rerun scan+DS3231   l = write LCD test pattern\n"));
}

void loop() {
    if (Serial.available()) {
        switch (Serial.read()) {
            case 'r': testI2C(); break;
            case 'l': testLcd();  break;
        }
    }
    delay(30);
}
