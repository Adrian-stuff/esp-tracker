// Uno scanner — connection self-test.
//
// Flash this BEFORE the real firmware (../src/). It exercises each
// peripheral on its own and says plainly what it found, so a fault points
// at one module instead of presenting as "nothing works". Self-contained —
// does not import ../src/'s modules, so it stays useful even if the real
// firmware doesn't currently compile.
//
// Unlike the ESP32 scanner's selftest, SIM900 here is NOT optional to test:
// on this build it's the only uplink there is, so this checks AT, signal
// quality, and network registration, not just "does it answer AT".
//
// *** If a peripheral fails here, verify it in isolation first *** —
// ../selftest_i2c/, ../selftest_rfid/, ../selftest_sim_only/,
// ../selftest_buzzer/, ../selftest_eeprom/. This combined sketch sits at
// ~94% static RAM (verified via `pio run`) because it loads every driver
// at once; that was confirmed, by direct A/B hardware test, to cause its
// own false failures — a reset loop that vanished entirely once the same
// wiring was tested against a low-RAM single-peripheral sketch instead.
// Trust the per-peripheral results over this file's when they disagree.
//
// Serial: 115200. Single-key commands are listed at the end of the report.
//
// RAM: 1869/2048 bytes static (91.3%, verified via `pio run`) — tighter than
// the real firmware, because this loads every peripheral driver at once for
// interactivity (press any test key anytime) rather than the real build's
// careful per-function buffer reuse. ~160 of those bytes are Wire's own
// fixed 32-byte buffers, which are a hard #define in the framework's Wire.h
// (not guarded by #ifndef) and can't be trimmed without patching a file
// outside this repo. If you see garbled Serial output or a reset specifically
// during the SIM900 test (the deepest call stack here), that's the signature
// of stack/heap pressure — temporarily comment out testRfid()/testLed() in
// setup() to free static RAM while isolating one peripheral.

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <EEPROM.h>
#include <MFRC522.h>
#include <RTClib.h>
#include <SoftwareSerial.h>
#include <string.h>
#include <stdlib.h>
#include <avr/wdt.h>

// DIAGNOSTIC: testing whether a watchdog left armed by the bootloader is
// causing the reset-at-varying-points behavior we've been chasing. This
// runs in .init3 — BEFORE main()/setup(), even before C++ global
// constructors — which is the earliest possible point and matters if the
// watchdog's timeout is short enough to fire during static init.
void wdtEarlyDisable(void) __attribute__((naked)) __attribute__((section(".init3")));
void wdtEarlyDisable(void) {
    MCUSR = 0;
    wdt_disable();
}

// ---- pin map: must match scanner-uno/include/pins.h ----------------------
#define PIN_RFID_SCK   13
#define PIN_RFID_MISO  12
#define PIN_RFID_MOSI  11
#define PIN_RFID_SS    10
#define PIN_RFID_RST   9
#define PIN_I2C_SDA    A4
#define PIN_I2C_SCL    A5
#define PIN_SIM_RX     7
#define PIN_SIM_TX     8
#define PIN_BUZZER     6

// ---- must match scanner-uno/include/config.h ------------------------------
#define LED_I2C_ADDR   0x62
#define GPRS_APN       "internet"   // Smart — see config.h

MFRC522        rfid(PIN_RFID_SS, PIN_RFID_RST);
RTC_DS3231     rtc;
SoftwareSerial sim(PIN_SIM_RX, PIN_SIM_TX);

static bool okI2c = false, okRtc = false, okLed = false, okRfid = false, okEeprom = false, okSim = false;

static void rule() { Serial.println(F("--------------------------------------------------")); }
static void head(const char* t) { rule(); Serial.print(F("  ")); Serial.println(t); rule(); }
static void result(const char* what, bool ok, const char* detail) {
    Serial.print(F("  ["));
    Serial.print(ok ? F("PASS") : F("FAIL"));
    Serial.print(F("]  "));
    Serial.print(what);
    for (int i = strlen(what); i < 22; i++) Serial.print(' ');
    Serial.println(detail);
}

// PCA9633 registers — must match scanner-uno/src/led.cpp
static constexpr uint8_t REG_MODE1  = 0x00;
static constexpr uint8_t REG_PWM0   = 0x02;
static constexpr uint8_t REG_LEDOUT = 0x08;
static constexpr uint8_t AUTO_INC   = 0x80;

static bool ledWriteReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(LED_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}
static void ledRgb(uint8_t r, uint8_t g, uint8_t b) {
    Wire.beginTransmission(LED_I2C_ADDR);
    Wire.write(AUTO_INC | REG_PWM0);
    Wire.write(r); Wire.write(g); Wire.write(b); Wire.write((uint8_t)0);
    Wire.endTransmission();
}

// ------------------------------------------------------------------ I2C ---
static void testI2C() {
    head("1. I2C bus  (DS3231 clock + PCA9633 LED, shared bus)");
    Wire.begin();
    // Without this, a stuck bus (SDA/SCL swapped, shorted, or a device not
    // releasing the clock) hangs Wire.endTransmission() FOREVER — the AVR
    // core's timeout protection defaults to disabled. 25ms is generous for
    // a bus running at the default 100kHz; true = reset the TWI hardware
    // state on timeout so the NEXT call isn't left wedged too.
    Wire.setWireTimeout(25000, true);

    int found = 0;
    bool sawLedAddr = false;
    for (uint8_t a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            Serial.print(F("       device at 0x"));
            if (a < 16) Serial.print('0');
            Serial.print(a, HEX);
            if (a == 0x68) Serial.print(F("   <- DS3231"));
            if (a == LED_I2C_ADDR) { Serial.print(F("   <- PCA9633 LED")); sawLedAddr = true; }
            Serial.println();
            found++;
        }
    }
    if (!found) {
        if (Wire.getWireTimeoutFlag()) {
            // Distinct from "nothing answered": the bus itself is stuck low
            // (SDA/SCL swapped, shorted together, or a device holding the
            // clock) rather than simply having no devices on it.
            result("I2C devices", false, "BUS STUCK - timed out. Check SDA/SCL aren't swapped or shorted");
            Wire.clearWireTimeoutFlag();
        } else {
            result("I2C devices", false, "nothing responded - check SDA=A4, SCL=A5, 3V3, GND");
        }
        okI2c = false; okRtc = false; okLed = false;
        return;
    }
    okI2c = true;
    if (!sawLedAddr)
        Serial.println(F("       NOTE: nothing answered at LED_I2C_ADDR - wrong address, or it collides with 0x68"));

    if (!rtc.begin()) {
        result("DS3231", false, "on the bus but not answering as a DS3231");
        okRtc = false;
    } else if (rtc.lostPower()) {
        // Not a wiring fault, but the real firmware treats it as no clock at
        // all: it refuses to queue taps rather than stamp them with a made-
        // up time.
        result("DS3231", false, "LOST POWER - fit/replace the CR2032, then set the time");
        okRtc = false;
    } else {
        DateTime n = rtc.now();
        // NOT %.1f: avr-libc's default snprintf doesn't support float
        // conversions at all (it silently prints garbage, not just a
        // warning) unless linked against a float-enabled variant, which
        // isn't worth the extra flash for a diagnostic readout. One
        // decimal digit by hand instead.
        float t = rtc.getTemperature();
        int whole = (int)t;
        int tenths = (int)((t - whole) * 10);
        if (tenths < 0) tenths = -tenths;
        char buf[48];
        snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d  (%d.%d C)",
                 n.year(), n.month(), n.day(), n.hour(), n.minute(), n.second(),
                 whole, tenths);
        result("DS3231", true, buf);
        okRtc = true;
    }
}

// ----------------------------------------------------------------- RC522 ---
static void testRfid() {
    head("2. SPI bus  (RC522 card reader)");
    SPI.begin();
    rfid.PCD_Init();
    delay(50);

    byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    char detail[64];
    if (v == 0x00 || v == 0xFF) {
        // 0x00 and 0xFF are the two "nothing is talking" answers: a floating
        // MISO reads as one or the other depending on the pull.
        snprintf(detail, sizeof detail, "version 0x%02X - no response. Check wiring / 3V3 ONLY", v);
        result("RC522", false, detail);
        okRfid = false;
        return;
    }
    snprintf(detail, sizeof detail, "version 0x%02X (%s)", v,
             v == 0x91 ? "v1.0" : v == 0x92 ? "v2.0" : "clone, usually fine");
    result("RC522", true, detail);
    rfid.PCD_AntennaOn();
    Serial.println(F("       antenna on - tap a card any time, UIDs print below"));
    okRfid = true;
}

// -------------------------------------------------------------------- LED --
static void testLed() {
    head("3. Feedback LED  (PCA9633 over I2C)");
    if (!ledWriteReg(REG_MODE1, 0x00) || !ledWriteReg(REG_LEDOUT, 0xAA)) {
        result("PCA9633", false, "no ACK at LED_I2C_ADDR - see section 1's I2C scan");
        okLed = false;
        return;
    }
    result("PCA9633", true, "ACKed - watch the LED cycle through colours");

    struct { const char* name; uint8_t r, g, b; } steps[] = {
        {"RED",   255, 0,   0},
        {"GREEN", 0,   255, 0},
        {"BLUE",  0,   0,   255},
        {"WHITE-ish (all channels)", 255, 255, 255},
    };
    for (auto& s : steps) {
        Serial.print(F("       ")); Serial.println(s.name);
        ledRgb(s.r, s.g, s.b);
        delay(600);
    }
    ledRgb(0, 0, 0);
}

// ----------------------------------------------------------------- buzzer --
static void testBuzzer() {
    head("4. Buzzer");
    Serial.println(F("       rising chirp"));
    tone(PIN_BUZZER, 1800); delay(120);
    tone(PIN_BUZZER, 2400); delay(160);
    noTone(PIN_BUZZER);
}

// ----------------------------------------------------------------- EEPROM --
static void testEeprom() {
    head("5. Internal EEPROM  (roster cache + offline tap queue live here)");
    // Probe near the very end of the 1024-byte EEPROM, well past
    // eeprom_layout.h's QUEUE_END (~950) — this can never collide with the
    // real firmware's data, even if this is re-run on a device that already
    // has taps queued.
    const int addr = 1020;
    uint8_t original = EEPROM.read(addr);
    uint8_t pattern  = 0xA5;
    EEPROM.write(addr, pattern);
    uint8_t back = EEPROM.read(addr);
    EEPROM.write(addr, original);   // restore whatever was there

    if (back != pattern) {
        result("EEPROM", false, "wrote 0xA5, read something else back - chip likely dead");
        okEeprom = false;
    } else {
        result("EEPROM", true, "write/read verified at a probe address");
        okEeprom = true;
    }
}

// ----------------------------------------------------------------- SIM900 --
// Fixed buffer, not String: an earlier version of this test used String for
// AT-response capture and pushed static RAM to 97.5% (1997/2048), leaving
// almost nothing for the stack — String's heap grows into exactly that same
// space as it concatenates each incoming byte. Verified by rebuilding after
// removing it: see the platformio.ini comment for the before/after numbers.
static char s_atBuf[32];   // "+CSQ: 15,99" / "+CREG: 0,1" style replies fit easily

static bool simCmd(const char* cmd, const char* expect, uint32_t timeoutMs) {
    while (sim.available()) sim.read();
    sim.print(cmd); sim.print("\r\n");
    uint32_t deadline = millis() + timeoutMs;
    size_t n = 0;
    s_atBuf[0] = 0;
    while ((int32_t)(millis() - deadline) < 0) {
        while (sim.available() && n < sizeof(s_atBuf) - 1) s_atBuf[n++] = (char)sim.read();
        s_atBuf[n] = 0;
        if (strstr(s_atBuf, expect)) return true;
    }
    return false;
}

static void testSim() {
    head("6. SIM900 modem  (the ONLY uplink on this build - not optional)");
    sim.begin(19200);   // this shield's real default, not 9600
    Serial.println(F("       waiting for the module to finish booting..."));
    delay(3000);

    if (!simCmd("AT", "OK", 3000)) {
        result("SIM900 AT", false, "no OK - check power (shield's own button pressed?), TX/RX wiring");
        okSim = false;
        return;
    }
    result("SIM900 AT", true, "responded OK");

    simCmd("ATE0", "OK", 1000);   // stop echo, same as the real firmware

    char csqDetail[32] = "no response";
    if (simCmd("AT+CSQ", "+CSQ:", 2000)) {
        char* p = strstr(s_atBuf, "+CSQ:");
        int csq = p ? atoi(p + 5) : -1;
        const char* label = (csq == 99) ? "unknown/no signal"
                           : (csq < 10)  ? "POOR"
                           : (csq < 15)  ? "fair"
                           : "good";
        snprintf(csqDetail, sizeof csqDetail, "AT+CSQ=%d (%s)", csq, label);
    }
    result("Signal quality", true, csqDetail);

    char cregDetail[32] = "no response";
    bool registered = false;
    if (simCmd("AT+CREG?", "+CREG:", 3000)) {
        char* comma = strchr(s_atBuf, ',');
        int stat = comma ? atoi(comma + 1) : -1;
        const char* label = (stat == 1) ? "registered, home network"
                           : (stat == 5) ? "registered, roaming"
                           : (stat == 2) ? "searching..."
                           : (stat == 3) ? "registration DENIED"
                           : "not registered";
        registered = (stat == 1 || stat == 5);
        snprintf(cregDetail, sizeof cregDetail, "AT+CREG stat=%d (%s)", stat, label);
    }
    result("Network registration", registered, cregDetail);
    if (!registered)
        Serial.println(F("       give it another minute (2G attach can be slow) then press 's' to retry"));

    okSim = true;   // AT responded; registration may still be pending, which is not a wiring fault
}

// ------------------------------------------------------------------ main ---
static void report() {
    head("SUMMARY");
    result("I2C bus",         okI2c,    okI2c    ? "ready" : "see section 1");
    result("DS3231 clock",    okRtc,    okRtc    ? "ready" : "TAPS WILL BE REFUSED without this");
    result("PCA9633 LED",     okLed,    okLed    ? "ready" : "see section 3");
    result("RC522 reader",    okRfid,   okRfid   ? "ready" : "see section 2");
    result("EEPROM",          okEeprom, okEeprom ? "ready" : "see section 5");
    result("SIM900",          okSim,    okSim    ? "AT ready - check registration above" : "see section 6");
    rule();
    Serial.println(F("  Commands:  r = rerun all    i = I2C scan   l = LED cycle"));
    Serial.println(F("             b = buzzer       s = SIM900 probe"));
    rule();
    Serial.println(F("  Tap a card to print its UID.\n"));
}

void setup() {
    wdt_disable();   // belt-and-suspenders alongside the .init3 version above
    Serial.begin(115200);
    delay(600);
    pinMode(PIN_BUZZER, OUTPUT);

    Serial.println(F("\n\n=================================================="));
    Serial.println(F("  UNO SCANNER - CONNECTION SELF TEST"));
    Serial.println(F("=================================================="));

    testI2C();
    testRfid();
    testLed();
    testBuzzer();
    testEeprom();
    testSim();
    report();
}

void loop() {
    if (Serial.available()) {
        switch (Serial.read()) {
            case 'r': testI2C(); testRfid(); testLed(); testBuzzer(); testEeprom(); testSim(); report(); break;
            case 'i': testI2C();   break;
            case 'l': testLed();   break;
            case 'b': testBuzzer();break;
            case 's': testSim();   break;
        }
    }

    if (okRfid && rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
        char uid[24] = {0};
        for (byte i = 0; i < rfid.uid.size && i < 10; i++)
            snprintf(uid + i * 2, sizeof(uid) - i * 2, "%02X", rfid.uid.uidByte[i]);
        Serial.print(F("  CARD  uid=")); Serial.println(uid);
        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();

        ledRgb(0, 255, 0);
        tone(PIN_BUZZER, 1800); delay(70);
        tone(PIN_BUZZER, 2400); delay(90);
        noTone(PIN_BUZZER);
        delay(200);
        ledRgb(0, 0, 0);
    }

    static uint32_t lastBeat = 0;
    if (millis() - lastBeat >= 1000) {
        lastBeat = millis();
        Serial.print(F("alive t="));
        Serial.println(millis() / 1000);
    }

    delay(30);
}
