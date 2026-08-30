// DS1302-only isolation test. 3-wire protocol (CE/IO/SCLK), NOT I2C — this
// is why it never showed up in any I2C bus scan earlier; wrong protocol,
// not a wiring fault. See scanner-uno/include/pins.h for the real pin
// assignment (CE=4, IO=3, SCLK=2) and clock.h for the important caveat:
// unlike a DS3231, this chip has no reliable hardware "I lost backup
// power" signal, only software-checkable sanity ranges.
//
// Commands: r = reread + print   s = SET the clock to this PC's compile
// time (__DATE__/__TIME__) — convenient for first bring-up, obviously not
// where real time should come from once this is wired to the modem's NITZ
// sync (see modem::syncClockFromNetwork in the real firmware).

#include <Arduino.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>

#define PIN_RTC_CE   4
#define PIN_RTC_IO   3
#define PIN_RTC_SCLK 2

ThreeWire            myWire(PIN_RTC_IO, PIN_RTC_SCLK, PIN_RTC_CE);
RtcDS1302<ThreeWire>  Rtc(myWire);

static void printNow() {
    RtcDateTime dt = Rtc.GetDateTime();
    char buf[32];
    snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u:%02u",
             dt.Year(), dt.Month(), dt.Day(), dt.Hour(), dt.Minute(), dt.Second());
    Serial.print(F("  current time: "));
    Serial.println(buf);
    Serial.print(F("  register-range valid: "));
    Serial.println(dt.IsValid() ? F("yes") : F("NO - garbage registers"));
}

static void report() {
    Serial.println(F("\n=== DS1302-ONLY isolation test ==="));
    Rtc.Begin();

    bool wp = Rtc.GetIsWriteProtected();
    Serial.print(F("write-protected: "));
    Serial.println(wp ? F("yes") : F("no"));

    bool running = Rtc.GetIsRunning();
    Serial.print(F("clock-halt bit says running: "));
    Serial.println(running ? F("yes") : F("NO - halted (normal on a fresh/unpowered chip until you set the time)"));

    printNow();

    Serial.println(F("\nCommands: r = reread   s = set to this PC's compile time\n"));
}

void setup() {
    Serial.begin(115200);
    delay(600);
    report();
}

void loop() {
    if (Serial.available()) {
        switch (Serial.read()) {
            case 'r':
                printNow();
                break;
            case 's': {
                RtcDateTime compiled(__DATE__, __TIME__);
                Rtc.SetIsWriteProtected(false);
                Rtc.SetDateTime(compiled);
                Rtc.SetIsRunning(true);
                Serial.println(F("  set to compile time - press 'r' to confirm it stuck"));
                break;
            }
        }
    }
    delay(30);
}
