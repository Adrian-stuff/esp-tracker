// LCD + DS1302 ONLY isolation test — bisecting whether the main firmware's
// crash (reset right inside display::begin(), immediately after
// Wire.begin(), reproducible every time with the full RC522+DS1302+LCD+
// buzzer+SIM800L harness connected) needs everything wired, or happens
// with just these two. Mirrors the real firmware's exact boot order:
// Wire.begin() -> LCD init -> DS1302 init.
//
// Pins match scanner-uno/include/pins.h: LCD on I2C (A4/A5, addr 0x27),
// DS1302 on CE=4/IO=3/SCLK=2.

#include <Arduino.h>
#include <Wire.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>
#include <LiquidCrystal_I2C.h>
#include <avr/wdt.h>

#define LCD_I2C_ADDR 0x27
#define LCD_COLS     16
#define LCD_ROWS     2
#define PIN_RTC_CE   4
#define PIN_RTC_IO   3
#define PIN_RTC_SCLK 2

void wdtEarlyDisable(void) __attribute__((naked)) __attribute__((section(".init3")));
void wdtEarlyDisable(void) { MCUSR = 0; wdt_disable(); }

LiquidCrystal_I2C    lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);
ThreeWire             myWire(PIN_RTC_IO, PIN_RTC_SCLK, PIN_RTC_CE);
RtcDS1302<ThreeWire>  Rtc(myWire);

void setup() {
    wdt_disable();
    Serial.begin(9600);
    delay(600);
    Serial.println(F("\nDIAG: boot start"));

    Wire.begin();
    Serial.println(F("DIAG: Wire.begin() done"));

    lcd.init();
    Serial.println(F("DIAG: lcd.init() done"));
    lcd.backlight();
    Serial.println(F("DIAG: lcd.backlight() done"));
    lcd.clear();
    Serial.println(F("DIAG: lcd.clear() done"));
    lcd.setCursor(0, 0);
    lcd.print("LCD+RTC test");
    Serial.println(F("DIAG: lcd.print() done"));

    Rtc.Begin();
    Serial.println(F("DIAG: Rtc.Begin() done"));
    bool running = Rtc.GetIsRunning();
    Serial.print(F("DIAG: Rtc.GetIsRunning() = "));
    Serial.println(running);

    Serial.println(F("DIAG: reached end of setup() cleanly - no crash"));
}

void loop() {
    static uint32_t last = 0;
    if (millis() - last >= 1000) {
        last = millis();
        Serial.print(F("alive t="));
        Serial.println(millis() / 1000);
    }
    delay(30);
}
