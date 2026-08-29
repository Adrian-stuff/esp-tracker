// Gate scanner — connection self-test.
//
// Flash this BEFORE the real firmware. It exercises each peripheral on its own
// and says plainly what it found, so a fault points at one module instead of
// presenting as "nothing works".
//
// It deliberately does NOT need the network, a server, or a SIM900. Anything
// not fitted is reported as absent rather than failed.
//
// Serial: 115200. Single-key commands are listed at the end of the report.

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include <RTClib.h>
#include <WiFi.h>

// ---- pin map: must match scanner/include/pins.h -------------------------
#define PIN_RFID_SS    5
#define PIN_RFID_SCK   18
#define PIN_RFID_MOSI  23
#define PIN_RFID_MISO  19
#define PIN_RFID_RST   4
#define PIN_I2C_SDA    21
#define PIN_I2C_SCL    22
#define PIN_SIM_RX     16
#define PIN_SIM_TX     17
#define PIN_SIM_PWRKEY 32
#define PIN_BUZZER     25
#define PIN_LED_OK     26   // green leg
#define PIN_LED_ERR    27   // red leg
#define PIN_LED_B      13   // blue leg (RGB only)

// Flip if your RGB LED lights when the pin is pulled LOW. The test tells you
// which one you have, so run it once and set this to match.
static constexpr bool LED_COMMON_ANODE = false;
static constexpr bool HAS_BLUE_LEG     = true;
static constexpr bool EXPECT_SIM900    = false;   // not fitted yet

MFRC522        rfid(PIN_RFID_SS, PIN_RFID_RST);
RTC_DS3231     rtc;
HardwareSerial Sim(2);

static bool okRfid = false, okRtc = false, okWifi = false, okSim = false;

static void led(uint8_t pin, bool on) {
    digitalWrite(pin, (on != LED_COMMON_ANODE) ? HIGH : LOW);
}
static void allLeds(bool on) {
    led(PIN_LED_OK, on); led(PIN_LED_ERR, on);
    if (HAS_BLUE_LEG) led(PIN_LED_B, on);
}
static void rule() { Serial.println(F("--------------------------------------------------")); }
static void head(const char* t) { rule(); Serial.printf("  %s\n", t); rule(); }
static void result(const char* what, bool ok, const char* detail) {
    Serial.printf("  [%s]  %-22s %s\n", ok ? "PASS" : "FAIL", what, detail);
}

// ------------------------------------------------------------------ I2C ---
static void testI2C() {
    head("1. I2C bus  (DS3231 real-time clock)");
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

    int found = 0;
    for (uint8_t a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            Serial.printf("       device at 0x%02X%s\n", a,
                a == 0x68 ? "   <- DS3231" : (a == 0x57 ? "   <- AT24C32 eeprom (on the same board)"
                                                        : (a == 0x3C ? "   <- SSD1306 OLED" : "")));
            found++;
        }
    }
    if (!found) {
        result("I2C devices", false, "nothing responded - check SDA=21, SCL=22, 3V3, GND");
        okRtc = false;
        return;
    }

    if (!rtc.begin()) {
        result("DS3231", false, "on the bus but not answering as a DS3231");
        return;
    }
    if (rtc.lostPower()) {
        // Not a wiring fault, but the firmware treats it as no clock at all:
        // it refuses to queue taps rather than stamp them with a made-up time.
        result("DS3231", false, "LOST POWER - fit/replace the CR2032, then set the time");
        okRtc = false;
    } else {
        DateTime n = rtc.now();
        char buf[48];
        snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d  (%.1f C)",
                 n.year(), n.month(), n.day(), n.hour(), n.minute(), n.second(),
                 rtc.getTemperature());
        result("DS3231", true, buf);
        okRtc = true;
    }
}

// ----------------------------------------------------------------- RC522 ---
static void testRfid() {
    head("2. SPI bus  (RC522 card reader)");
    SPI.begin(PIN_RFID_SCK, PIN_RFID_MISO, PIN_RFID_MOSI, PIN_RFID_SS);
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

// ------------------------------------------------------------ LEDs/buzzer ---
static void testOutputs() {
    head("3. Feedback  (LEDs + buzzer)");
    Serial.println(F("       watch the LED - each colour lights for ~0.6 s"));

    struct { uint8_t pin; const char* name; } legs[] = {
        { PIN_LED_ERR, "RED   (GPIO 27)" },
        { PIN_LED_OK,  "GREEN (GPIO 26)" },
        { PIN_LED_B,   "BLUE  (GPIO 13)" },
    };
    for (auto& l : legs) {
        if (l.pin == PIN_LED_B && !HAS_BLUE_LEG) continue;
        Serial.printf("       %s\n", l.name);
        allLeds(false); led(l.pin, true); delay(600);
    }
    allLeds(false);

    Serial.println(F("       all three together (should look white-ish on an RGB)"));
    allLeds(true); delay(700); allLeds(false);

    Serial.println(F("       buzzer: rising chirp"));
    tone(PIN_BUZZER, 1800); delay(120);
    tone(PIN_BUZZER, 2400); delay(160);
    noTone(PIN_BUZZER);

    Serial.println(F("\n       If the LED was LIT between colours and DARK during them,"));
    Serial.println(F("       you have a COMMON ANODE part: set LED_COMMON_ANODE = true"));
    Serial.println(F("       here and in scanner/include/config.h."));
}

// ------------------------------------------------------------------ WiFi ---
static void testWifi() {
    head("4. Wi-Fi radio");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    int n = WiFi.scanNetworks();
    if (n <= 0) {
        result("Wi-Fi scan", false, "no networks seen - unusual indoors, check the antenna");
        okWifi = false;
        return;
    }
    char detail[48];
    snprintf(detail, sizeof detail, "%d networks visible", n);
    result("Wi-Fi scan", true, detail);
    for (int i = 0; i < n && i < 6; i++)
        Serial.printf("       %-24s %4d dBm\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i));
    // Wi-Fi doubles as the indoor position source and the motion gate, so a
    // healthy scan here matters beyond just reaching the server.
    WiFi.scanDelete();
    okWifi = true;
}

// ---------------------------------------------------------------- SIM900 ---
static void testSim() {
    head("5. SIM900 modem");
    if (!EXPECT_SIM900) {
        Serial.println(F("       skipped - EXPECT_SIM900 is false."));
        Serial.println(F("       The scanner runs fine without it: taps go out over"));
        Serial.println(F("       Wi-Fi and the SERVER texts the parent instead."));
        okSim = true;                     // absent by choice is not a failure
        return;
    }
    pinMode(PIN_SIM_PWRKEY, OUTPUT);
    digitalWrite(PIN_SIM_PWRKEY, LOW);  delay(1200);
    digitalWrite(PIN_SIM_PWRKEY, HIGH);
    Sim.begin(9600, SERIAL_8N1, PIN_SIM_RX, PIN_SIM_TX);
    delay(3000);

    Sim.println("AT");
    uint32_t t0 = millis(); String r;
    while (millis() - t0 < 3000) { while (Sim.available()) r += (char)Sim.read(); }
    if (r.indexOf("OK") < 0) {
        result("SIM900 AT", false, "no OK - PWRKEY not pulsed, TX/RX swapped, or power");
        okSim = false;
        return;
    }
    result("SIM900 AT", true, "responded OK");
    okSim = true;
}

// ------------------------------------------------------------------ main ---
static void report() {
    head("SUMMARY");
    result("RC522 reader", okRfid, okRfid ? "ready" : "see section 2");
    result("DS3231 clock", okRtc,  okRtc  ? "ready" : "TAPS WILL BE REFUSED without this");
    result("Wi-Fi", okWifi, okWifi ? "ready" : "see section 4");
    result("SIM900", okSim, EXPECT_SIM900 ? (okSim ? "ready" : "see section 5") : "not fitted (fine)");
    rule();
    Serial.println(F("  Commands:  r = rerun all    i = I2C scan    l = LED cycle"));
    Serial.println(F("             b = buzzer       w = Wi-Fi scan  s = SIM900 probe"));
    rule();
    Serial.println(F("  Tap a card to print its UID.\n"));
}

void setup() {
    Serial.begin(115200);
    delay(600);
    pinMode(PIN_LED_OK, OUTPUT); pinMode(PIN_LED_ERR, OUTPUT);
    pinMode(PIN_LED_B, OUTPUT);  pinMode(PIN_BUZZER, OUTPUT);
    allLeds(false);

    Serial.println(F("\n\n=================================================="));
    Serial.println(F("  GATE SCANNER - CONNECTION SELF TEST"));
    Serial.println(F("=================================================="));

    testI2C();
    testRfid();
    testOutputs();
    testWifi();
    testSim();
    report();
}

void loop() {
    if (Serial.available()) {
        switch (Serial.read()) {
            case 'r': testI2C(); testRfid(); testOutputs(); testWifi(); testSim(); report(); break;
            case 'i': testI2C();    break;
            case 'l': testOutputs();break;
            case 'w': testWifi();   break;
            case 's': testSim();    break;
            case 'b': tone(PIN_BUZZER, 2200); delay(200); noTone(PIN_BUZZER); break;
        }
    }

    if (okRfid && rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
        char uid[24] = {0};
        for (byte i = 0; i < rfid.uid.size && i < 10; i++)
            snprintf(uid + i * 2, sizeof(uid) - i * 2, "%02X", rfid.uid.uidByte[i]);
        Serial.printf("  CARD  uid=%s  type=%s\n", uid,
            rfid.PICC_GetTypeName(rfid.PICC_GetType(rfid.uid.sak)));
        Serial.println(F("        ^ paste this into the cards table to enrol it"));
        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();

        led(PIN_LED_OK, true);
        tone(PIN_BUZZER, 1800); delay(70);
        tone(PIN_BUZZER, 2400); delay(90);
        noTone(PIN_BUZZER);
        delay(200);
        led(PIN_LED_OK, false);
    }
    delay(30);
}
