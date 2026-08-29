// Phase 0 coverage survey.
//
// Answers the two questions that decide whether this project is buildable at
// all, and both are cheap to answer now and ruinous to discover in Phase 3:
//
//   1. Does 2G actually register, with usable signal, along the child's real
//      route? (Run it twice — once on a Globe SIM, once on Smart.)
//   2. Are there enough Wi-Fi APs indoors to get a position where GNSS cannot?
//
// Walk the route with this on a USB power bank. No laptop needed: samples are
// written to flash, and you dump them over serial afterwards.
//
//   Normal boot            -> logging. LED blinks once per sample.
//   Boot with button held  -> dumps survey.csv to serial, then stops.
//   Button press (logging) -> marks a waypoint. Press at the school gate, in
//                             the classroom, at home — anywhere the answer
//                             matters more than the average.
//   Button held 3 s        -> erases the log (LED flashes fast 5x first).

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <TinyGPSPlus.h>

// ---- pins: same map as the tracker, so the survey rig IS the prototype ----
#define PIN_MODEM_RX  16
#define PIN_MODEM_TX  17     // *** level-shift this line to the SIM800L ***
#define PIN_MODEM_PWRKEY 23
#define PIN_GPS_RX    25
#define PIN_GPS_TX    26
#define PIN_BUTTON    33     // active low
#define PIN_LED        2

static const uint32_t SAMPLE_MS   = 10000;
static const uint8_t  MAX_APS     = 6;     // enough for a geolocation query
static const char*    LOG_PATH    = "/survey.csv";

HardwareSerial Modem(2);
HardwareSerial Gps(1);
TinyGPSPlus gps;

static uint16_t waypoint = 0;
static uint32_t sampleNo = 0;

// --------------------------------------------------------------- helpers ---
static String sendAT(const char* cmd, uint32_t timeout = 2000) {
    while (Modem.available()) Modem.read();
    Modem.println(cmd);
    String out;
    uint32_t t0 = millis();
    while (millis() - t0 < timeout) {
        while (Modem.available()) out += (char)Modem.read();
        if (out.indexOf("OK") >= 0 || out.indexOf("ERROR") >= 0) break;
    }
    out.replace("\r", " "); out.replace("\n", " ");
    out.trim();
    return out;
}

// "+CSQ: 17,0" -> 17. 99 means "not known or not detectable".
static int parseCsq(const String& r) {
    int i = r.indexOf("+CSQ:");
    if (i < 0) return -1;
    return r.substring(i + 5, r.indexOf(',', i)).toInt();
}

// "+CREG: 0,1" -> 1 (home) or 5 (roaming) mean registered.
static int parseCreg(const String& r) {
    int i = r.indexOf("+CREG:");
    if (i < 0) return -1;
    int c = r.indexOf(',', i);
    return c < 0 ? -1 : r.substring(c + 1, c + 2).toInt();
}

static String parseOperator(const String& r) {
    int q1 = r.indexOf('"');
    int q2 = r.indexOf('"', q1 + 1);
    return (q1 < 0 || q2 < 0) ? String("") : r.substring(q1 + 1, q2);
}

static void blink(int times, int on = 60, int off = 120) {
    for (int i = 0; i < times; i++) {
        digitalWrite(PIN_LED, HIGH); delay(on);
        digitalWrite(PIN_LED, LOW);  delay(off);
    }
}

// ------------------------------------------------------------- dump mode ---
static void dumpAndHalt() {
    Serial.println(F("\n===== survey.csv ====="));
    File f = LittleFS.open(LOG_PATH, "r");
    if (!f) { Serial.println(F("(no log yet)")); }
    else { while (f.available()) Serial.write(f.read()); f.close(); }
    Serial.println(F("\n===== end ====="));
    Serial.println(F("Save the block above as survey.csv, then: python3 analyze.py survey.csv"));
    while (true) { blink(1, 400, 1600); }
}

// ---------------------------------------------------------------- sample ---
static void takeSample() {
    // --- GNSS: feed whatever has arrived since the last sample -------------
    uint32_t t0 = millis();
    while (millis() - t0 < 400) { while (Gps.available()) gps.encode(Gps.read()); }
    bool   fix  = gps.location.isValid() && gps.location.age() < 10000;
    double lat  = fix ? gps.location.lat() : 0.0;
    double lon  = fix ? gps.location.lng() : 0.0;
    int    sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
    double hdop = gps.hdop.isValid() ? gps.hdop.hdop() : 0.0;

    // --- 2G ----------------------------------------------------------------
    int    csq  = parseCsq(sendAT("AT+CSQ"));
    int    creg = parseCreg(sendAT("AT+CREG?"));
    String op   = parseOperator(sendAT("AT+COPS?", 3000));

    // --- Wi-Fi: the indoor position source. Passive scan, no association. ---
    int n = WiFi.scanNetworks(false /* sync */, false /* show hidden */);
    String aps;
    for (int i = 0; i < n && i < MAX_APS; i++) {
        if (i) aps += ";";
        aps += WiFi.BSSIDstr(i) + "/" + String(WiFi.RSSI(i));
    }
    WiFi.scanDelete();

    char line[512];
    snprintf(line, sizeof line,
             "%lu,%u,%u,%d,%d,%.6f,%.6f,%.1f,%d,%d,\"%s\",%d,\"%s\"\n",
             (unsigned long)millis(), (unsigned)++sampleNo, (unsigned)waypoint,
             fix ? 1 : 0, sats, lat, lon, hdop, creg, csq, op.c_str(), n, aps.c_str());

    File f = LittleFS.open(LOG_PATH, "a");
    if (f) { f.print(line); f.close(); }
    Serial.print(line);

    // Signal quality at a glance, no screen required:
    //   3 blinks = registered with decent signal
    //   2 blinks = registered but weak (CSQ < 10)
    //   1 blink  = NOT registered  <-- this is the finding that matters
    bool registered = (creg == 1 || creg == 5);
    blink(!registered ? 1 : (csq >= 10 && csq != 99 ? 3 : 2));
}

// ------------------------------------------------------------------ main ---
void setup() {
    Serial.begin(115200);
    pinMode(PIN_LED, OUTPUT);
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    delay(300);

    if (!LittleFS.begin(true)) { Serial.println(F("LittleFS mount failed")); }

    if (digitalRead(PIN_BUTTON) == LOW) dumpAndHalt();

    if (!LittleFS.exists(LOG_PATH)) {
        File f = LittleFS.open(LOG_PATH, "w");
        if (f) { f.print(F("ms,sample,waypoint,gps_fix,sats,lat,lon,hdop,creg,csq,operator,ap_count,aps\n")); f.close(); }
    }

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();          // scanning only, never associating

    Gps.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

    Modem.begin(9600, SERIAL_8N1, PIN_MODEM_RX, PIN_MODEM_TX);
    pinMode(PIN_MODEM_PWRKEY, OUTPUT);
    digitalWrite(PIN_MODEM_PWRKEY, LOW);  delay(1100);
    digitalWrite(PIN_MODEM_PWRKEY, HIGH);
    delay(3000);
    sendAT("AT");
    sendAT("ATE0");             // no echo, keeps the parsing simple
    sendAT("AT+CMEE=2");        // verbose errors

    Serial.println(F("\nSurvey started. Walk the route."));
    Serial.println(F("Press the button to mark a waypoint (gate, classroom, home)."));
    Serial.println(F("Reboot with the button held to dump the log.\n"));
    blink(3, 150, 150);
}

void loop() {
    static uint32_t last = 0;
    static uint32_t heldSince = 0;

    // Button: short press marks a waypoint, 3 s hold erases the log.
    if (digitalRead(PIN_BUTTON) == LOW) {
        if (!heldSince) heldSince = millis();
        else if (millis() - heldSince > 3000) {
            blink(5, 60, 60);
            LittleFS.remove(LOG_PATH);
            sampleNo = 0; waypoint = 0;
            File f = LittleFS.open(LOG_PATH, "w");
            if (f) { f.print(F("ms,sample,waypoint,gps_fix,sats,lat,lon,hdop,creg,csq,operator,ap_count,aps\n")); f.close(); }
            Serial.println(F("log erased"));
            while (digitalRead(PIN_BUTTON) == LOW) delay(50);
            heldSince = 0;
        }
    } else if (heldSince) {
        if (millis() - heldSince < 3000) {
            waypoint++;
            Serial.printf("--- waypoint %u ---\n", waypoint);
            blink(2, 200, 150);
        }
        heldSince = 0;
    }

    while (Gps.available()) gps.encode(Gps.read());

    if (millis() - last >= SAMPLE_MS) { last = millis(); takeSample(); }
}
