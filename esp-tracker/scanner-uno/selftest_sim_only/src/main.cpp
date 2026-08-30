// SIM800L functionality test — dedicated, comprehensive.
//
// Deliberately loads NOTHING else (no RTClib, no MFRC522, no Wire) — only
// SoftwareSerial — so this has huge RAM headroom and is never at risk of
// the stack-margin crash that hit the full integrated firmware. If
// something fails here, it's the module/SIM/network, not this test.
//
// Pins: RX=7 (SIM800L TX), TX=8 (SIM800L RX, via level shifter). Baud:
// 9600, confirmed against real hardware — see scanner-uno/include/pins.h.
//
// Boot runs the full check sequence once automatically. Commands:
//   r = rerun full sequence     a = AT ping only        i = SIM/network info
//   s = send test SMS           c = signal quality only

#include <Arduino.h>
#include <SoftwareSerial.h>

#define PIN_SIM_RX 7
#define PIN_SIM_TX 8
#define SIM_BAUD   9600

#define TEST_SMS_NUMBER "+639109943152"
#define TEST_SMS_TEXT   "Scanner SMS test OK"

SoftwareSerial sim(PIN_SIM_RX, PIN_SIM_TX);

static void rule() { Serial.println(F("--------------------------------------------------")); }
static void head(const char* t) { rule(); Serial.print(F("  ")); Serial.println(t); rule(); }

// Sends one AT command, returns everything received within timeoutMs.
static String atQuery(const char* cmd, uint32_t timeoutMs = 2000) {
    while (sim.available()) sim.read();
    sim.print(cmd);
    sim.print("\r\n");
    uint32_t deadline = millis() + timeoutMs;
    String r;
    while ((int32_t)(millis() - deadline) < 0) {
        while (sim.available()) r += (char)sim.read();
    }
    return r;
}

static bool contains(const String& r, const char* needle) { return r.indexOf(needle) >= 0; }

// ------------------------------------------------------------------ AT ---
static bool testAt() {
    head("1. AT - basic command link");
    String r = atQuery("AT", 2000);
    bool ok = contains(r, "OK");
    Serial.println(ok ? F("  PASS - module responds") : F("  FAIL - no response. Check TX/RX/GND/power"));
    return ok;
}

// -------------------------------------------------------------- Module ---
static void testIdentity() {
    head("2. Module identity");
    String model = atQuery("AT+CGMM", 2000);
    Serial.print(F("  model: ")); Serial.println(model);
    String imei = atQuery("AT+CGSN", 2000);
    Serial.print(F("  IMEI:  ")); Serial.println(imei);
}

// ------------------------------------------------------------------ SIM ---
static bool testSim() {
    head("3. SIM card (AT+CPIN?)");
    String r = atQuery("AT+CPIN?", 3000);
    Serial.print(F("  raw: [")); Serial.print(r); Serial.println(F("]"));
    if (contains(r, "READY")) {
        Serial.println(F("  PASS - SIM present, no PIN needed"));
        return true;
    }
    if (contains(r, "SIM PIN") || contains(r, "SIM PUK")) {
        Serial.println(F("  SIM present but LOCKED - needs a PIN entered (AT+CPIN=\"1234\")"));
        return false;
    }
    Serial.println(F("  FAIL - module can't see a SIM at all. Check the SIM is inserted and fully seated,"));
    Serial.println(F("         inserted the right way up, and is a normal-size SIM cut down cleanly"));
    Serial.println(F("         (a badly-cut micro/nano adapter is a common cause of exactly this)."));
    return false;
}

// --------------------------------------------------------------- Signal ---
static void testSignal() {
    head("4. Signal quality (AT+CSQ)");
    String r = atQuery("AT+CSQ", 2000);
    int idx = r.indexOf("+CSQ:");
    if (idx < 0) { Serial.println(F("  no response")); return; }
    int csq = r.substring(idx + 5).toInt();
    const char* label = (csq == 99) ? "unknown/no signal"
                       : (csq < 10)  ? "POOR"
                       : (csq < 15)  ? "fair"
                       : "good";
    Serial.print(F("  AT+CSQ=")); Serial.print(csq); Serial.print(F(" (")); Serial.print(label);
    Serial.println(F(") - this works even without a SIM, it's just the radio's own signal meter"));
}

// --------------------------------------------------------- Registration ---
static bool testRegistration() {
    head("5. Network registration (AT+CREG?)");
    String r = atQuery("AT+CREG?", 3000);
    int comma = r.indexOf(',');
    int stat = comma >= 0 ? r.substring(comma + 1).toInt() : -1;
    const char* label = (stat == 1) ? "registered, home network"
                       : (stat == 5) ? "registered, roaming"
                       : (stat == 2) ? "searching..."
                       : (stat == 3) ? "registration DENIED"
                       : (stat == 0) ? "not registered, not searching"
                       : "unknown";
    bool ok = (stat == 1 || stat == 5);
    Serial.print(F("  AT+CREG stat=")); Serial.print(stat); Serial.print(F(" (")); Serial.print(label);
    Serial.println(F(")"));
    if (!ok && stat == 2) Serial.println(F("  Actively searching - give it another 10-30s and press 'i' again"));
    else if (!ok && stat != 0) Serial.println(F("  Needs a working SIM first (see section 3) - registration can't succeed without one"));
    return ok;
}

// -------------------------------------------------------------------SMS---
static bool testSms(const char* number, const char* text) {
    head("6. SMS send");
    atQuery("AT+CMGF=1", 1000);   // text mode

    char cmd[40];
    snprintf(cmd, sizeof cmd, "AT+CMGS=\"%s\"", number);
    while (sim.available()) sim.read();
    sim.print(cmd); sim.print("\r\n");
    uint32_t deadline = millis() + 3000;
    bool sawPrompt = false;
    while ((int32_t)(millis() - deadline) < 0 && !sawPrompt)
        if (sim.available() && sim.read() == '>') sawPrompt = true;
    if (!sawPrompt) {
        Serial.println(F("  FAIL - no '>' prompt from AT+CMGS. Needs a working, registered SIM (see sections 3+5)"));
        return false;
    }

    sim.print(text);
    sim.write(0x1A);   // Ctrl-Z sends it
    deadline = millis() + 15000;
    String r;
    while ((int32_t)(millis() - deadline) < 0) {
        while (sim.available()) r += (char)sim.read();
        if (contains(r, "OK") || contains(r, "ERROR")) break;
    }
    Serial.print(F("  result: [")); Serial.print(r); Serial.println(F("]"));
    bool ok = contains(r, "OK");
    Serial.println(ok ? F("  PASS - sent") : F("  FAIL - see result above"));
    return ok;
}

// ------------------------------------------------------------------ all ---
static void runAll() {
    Serial.println(F("\n=== SIM800L FUNCTIONALITY TEST ==="));
    if (!testAt()) { Serial.println(F("\nStopping here - nothing else will work without basic AT.")); return; }
    testIdentity();
    bool simOk = testSim();
    testSignal();
    bool netOk = testRegistration();
    if (simOk && netOk) {
        testSms(TEST_SMS_NUMBER, TEST_SMS_TEXT);
    } else {
        head("6. SMS send");
        Serial.println(F("  SKIPPED - needs both a working SIM and network registration first"));
    }
    rule();
    Serial.println(F("Commands: r=rerun all  a=AT only  i=SIM/network info  s=send SMS  c=signal only\n"));
}

void setup() {
    Serial.begin(115200);
    delay(600);
    sim.begin(SIM_BAUD);
    delay(3000);   // module boot time
    runAll();
}

void loop() {
    if (Serial.available()) {
        switch (Serial.read()) {
            case 'r': runAll(); break;
            case 'a': testAt(); break;
            case 'i': testSim(); testSignal(); testRegistration(); break;
            case 's': testSms(TEST_SMS_NUMBER, TEST_SMS_TEXT); break;
            case 'c': testSignal(); break;
        }
    }
    delay(30);
}
