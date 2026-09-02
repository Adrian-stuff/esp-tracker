#include <Arduino.h>
#include <sys/time.h>
#include <time.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include "../include/pins.h"
#include "../include/config.h"
#include "modem.h"
#include "gps.h"
#include "locator.h"
#include "sos.h"
#include "store.h"
#include "motion.h"
#include "feedback.h"
#include "notify.h"
#include "report.h"
#include "wifi_setup.h"

// Persistent storage for BLE-configurable values
static Preferences s_prefs;
char s_sosNumber[20];
char s_scannerNumber[20];

static void loadNumbers() {
    s_prefs.begin("tracker", true);  // read-only
    String sos = s_prefs.getString("sos_number", SOS_SMS_PRIMARY);
    String scanner = s_prefs.getString("scanner_number", SCANNER_SMS_NUMBER);
    s_prefs.end();
    strncpy(s_sosNumber, sos.c_str(), sizeof(s_sosNumber) - 1);
    strncpy(s_scannerNumber, scanner.c_str(), sizeof(s_scannerNumber) - 1);
    s_sosNumber[sizeof(s_sosNumber) - 1] = '\0';
    s_scannerNumber[sizeof(s_scannerNumber) - 1] = '\0';
}

static void saveSosNumber(const char* num) {
    s_prefs.begin("tracker", false);
    s_prefs.putString("sos_number", num);
    s_prefs.end();
    strncpy(s_sosNumber, num, sizeof(s_sosNumber) - 1);
    s_sosNumber[sizeof(s_sosNumber) - 1] = '\0';
}

static void saveScannerNumber(const char* num) {
    s_prefs.begin("tracker", false);
    s_prefs.putString("scanner_number", num);
    s_prefs.end();
    strncpy(s_scannerNumber, num, sizeof(s_scannerNumber) - 1);
    s_scannerNumber[sizeof(s_scannerNumber) - 1] = '\0';
}

// BLE Serial — wireless debug output + command interface
static NimBLEServer*  s_bleServer   = nullptr;
static NimBLECharacteristic* s_bleTx = nullptr;
static NimBLECharacteristic* s_bleRx = nullptr;
static bool           s_bleConnected = false;

// Small ring buffer so messages are not lost
static constexpr size_t BLE_BUF_SIZE = 512;
static char s_bleBuf[BLE_BUF_SIZE];
static size_t s_bleBufHead = 0;
static size_t s_bleBufLen  = 0;

static void bleBufAppend(const char* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        s_bleBuf[(s_bleBufHead + s_bleBufLen) % BLE_BUF_SIZE] = data[i];
        if (s_bleBufLen < BLE_BUF_SIZE) s_bleBufLen++;
        else s_bleBufHead = (s_bleBufHead + 1) % BLE_BUF_SIZE;
    }
}

static void bleFlush() {
    if (!s_bleConnected || !s_bleTx || s_bleBufLen == 0) return;
    while (s_bleBufLen > 0) {
        size_t chunk = s_bleBufLen > 20 ? 20 : s_bleBufLen;
        char tmp[21];
        for (size_t i = 0; i < chunk; i++) {
            tmp[i] = s_bleBuf[(s_bleBufHead + i) % BLE_BUF_SIZE];
        }
        tmp[chunk] = '\0';
        s_bleTx->setValue((uint8_t*)tmp, chunk);
        s_bleTx->notify();
        s_bleBufHead = (s_bleBufHead + chunk) % BLE_BUF_SIZE;
        s_bleBufLen -= chunk;
        delay(20);
    }
}

class BleServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* p) override {
        s_bleConnected = true;
        Serial.println("[BLE] client connected");
        // Flush any queued messages after a short delay for subscription setup
        delay(500);
        bleFlush();
    }
    void onDisconnect(NimBLEServer* p) override {
        s_bleConnected = false;
        Serial.println("[BLE] client disconnected");
        NimBLEDevice::startAdvertising();
    }
};
static BleServerCallbacks s_bleCbs;

// Parse and execute BLE commands
static void processBleCommand(const char* cmd) {
    char buf[128];
    snprintf(buf, sizeof buf, "[BLE] cmd: %s\n", cmd);
    Serial.print(buf);

    if (strncmp(cmd, "SOS ", 4) == 0) {
        const char* num = cmd + 4;
        if (strlen(num) >= 10 && strlen(num) <= 15) {
            saveSosNumber(num);
            snprintf(buf, sizeof buf, "SOS number set to %s\n", s_sosNumber);
        } else {
            snprintf(buf, sizeof buf, "Invalid number (need 10-15 digits)\n");
        }
    } else if (strncmp(cmd, "SCANNER ", 8) == 0) {
        const char* num = cmd + 8;
        if (strlen(num) >= 10 && strlen(num) <= 15) {
            saveScannerNumber(num);
            snprintf(buf, sizeof buf, "Scanner number set to %s\n", s_scannerNumber);
        } else {
            snprintf(buf, sizeof buf, "Invalid number (need 10-15 digits)\n");
        }
    } else if (strcmp(cmd, "STATUS") == 0) {
        int8_t csq = modem::signalQuality();
        int8_t creg = modem::networkStatus();
        snprintf(buf, sizeof buf,
            "SOS: %s\nScanner: %s\nCSQ: %d\nCREG: %d\n",
            s_sosNumber, s_scannerNumber, csq, creg);
    } else if (strcmp(cmd, "WIFI") == 0) {
        snprintf(buf, sizeof buf, "Entering WiFi config mode...\n");
        bleBufAppend(buf, strlen(buf));
        bleFlush();
        Serial.print(buf);
        delay(200);
        wifi_setup::enter();  // blocks, then reboots
    } else if (strcmp(cmd, "HELP") == 0) {
        snprintf(buf, sizeof buf,
            "Commands:\n"
            "  SOS +639XXXXXXXXX    - set SOS number\n"
            "  SCANNER +639XXXXXXXXX - set scanner number\n"
            "  STATUS               - show current config\n"
            "  WIFI                 - enter WiFi config mode\n"
            "  HELP                 - this help\n");
    } else {
        snprintf(buf, sizeof buf, "Unknown cmd. Type HELP\n");
    }

    // Send response over BLE (queued, sent when subscribed)
    bleBufAppend(buf, strlen(buf));
    bleFlush();
    Serial.print(buf);
}

// BLE RX callback — receives commands from phone
class BleRxCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* p) override {
        std::string val = p->getValue();
        if (val.length() > 0) {
            // Trim newline/carriage return
            char cmd[128];
            strncpy(cmd, val.c_str(), sizeof(cmd) - 1);
            cmd[sizeof(cmd) - 1] = '\0';
            size_t len = strlen(cmd);
            while (len > 0 && (cmd[len-1] == '\n' || cmd[len-1] == '\r')) cmd[--len] = '\0';
            if (len > 0) processBleCommand(cmd);
        }
    }
};
static BleRxCallback s_bleRxCbs;

static void bleInit() {
    NimBLEDevice::init("ESP-Tracker");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    s_bleServer = NimBLEDevice::createServer();
    s_bleServer->setCallbacks(&s_bleCbs);

    // Service with enough handles for TX + RX + CCCDs
    NimBLEService* svc = s_bleServer->createService("FFF0");

    // TX: device -> phone (notifications)
    s_bleTx = svc->createCharacteristic("FFF1",
        NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ, 256);
    s_bleTx->setValue("ESP-Tracker ready\n");

    // RX: phone -> device (writes)
    s_bleRx = svc->createCharacteristic("FFF2",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR, 128);
    s_bleRx->setCallbacks(&s_bleRxCbs);

    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID("FFF0");
    adv->setScanResponse(true);
    adv->start();
    Serial.println("[BLE] advertising as 'ESP-Tracker'");
}

// Dual output: Serial (USB) + BLE
static void dbg(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    Serial.print(buf);
    bleBufAppend(buf, strlen(buf));
    bleFlush();
}

// Tracker — ESP32 + SIM800L + NEO-6M + button.
//
// Build order note: this is Phase 01 from PLAN.md. The SOS path is the only
// thing that has to be right before anything else gets built on top of it.

// ---- TEST MODE: SMS every 5 seconds with full diagnostic ------------------
// Set to false to return to normal operation after testing.
static constexpr bool     TEST_MODE           = false;
static constexpr uint32_t TEST_SMS_INTERVAL_MS = 5000;
static constexpr const char* TEST_SMS_NUMBER  = "+639109943152";
static uint32_t s_testCounter = 0;
static uint32_t s_lastTestSms = 0;

static void sendTestSms() {
    s_testCounter++;

    int8_t csq = modem::signalQuality();
    int8_t creg = modem::networkStatus();

    // Get timestamp from modem clock
    char ts[20] = "??";
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm* t = localtime(&tv.tv_sec);
    strftime(ts, sizeof ts, "%H:%M:%S", t);

    // Build diagnostic message
    char body[160];
    snprintf(body, sizeof body,
        "[TEST #%lu] %s | CSQ:%d CREG:%d",
        (unsigned long)s_testCounter, ts,
        csq, creg);

    dbg("TEST SMS: %s\n", body);

    if (modem::sendSms(TEST_SMS_NUMBER, body)) {
        dbg("  -> SMS sent OK\n");
    } else {
        dbg("  -> SMS FAILED\n");
    }
}

// Battery estimation from uptime (no ADC hardware).
// Assumes a 2500mAh LiPo with average ~5mA draw (CSCLK sleep + periodic scans).
// This is a rough estimate — real battery life depends heavily on usage patterns.
// Reset the counter when the device is known to be freshly charged.
static uint32_t s_bootMs = 0;
static constexpr float BATTERY_CAPACITY_MAH = 2500.0f;
static constexpr float AVG_DRAW_MA = 5.0f;      // conservative estimate
static constexpr float BATTERY_WARN_PCT = 20.0f;
static constexpr float BATTERY_CRIT_PCT = 10.0f;
static bool s_batteryWarned = false;
static bool s_batteryCrit = false;

static uint8_t estimateBatteryPct() {
    uint32_t hoursUp = (millis() - s_bootMs) / 3600000UL;
    float used_mAh = hoursUp * AVG_DRAW_MA;
    float pct = 100.0f * (1.0f - used_mAh / BATTERY_CAPACITY_MAH);
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return (uint8_t)pct;
}

static void checkBattery() {
    uint8_t pct = estimateBatteryPct();
    if (pct <= BATTERY_CRIT_PCT && !s_batteryCrit) {
        s_batteryCrit = true;
        notify::fire(Event::BatteryCritical, nullptr);
        feedback::play(Cue::LowBattery);
    } else if (pct <= BATTERY_WARN_PCT && !s_batteryWarned) {
        s_batteryWarned = true;
        notify::fire(Event::BatteryLow, nullptr);
        feedback::play(Cue::LowBattery);
    }
}

static uint32_t s_buttonDownAt = 0;

static void serviceButton() {
    bool down = (digitalRead(PIN_SOS_BUTTON) == LOW);

    if (down && s_buttonDownAt == 0) {
        s_buttonDownAt = millis();
    } else if (!down) {
        s_buttonDownAt = 0;
    } else if (sos::active() && millis() - s_buttonDownAt >= SOS_HOLD_MS) {
        // Second 2s hold during the cancel window aborts an accidental press.
        sos::cancel();
        s_buttonDownAt = 0;
    } else if (!sos::active() && millis() - s_buttonDownAt >= SOS_HOLD_MS) {
        // First 2s hold, never a tap: pocket false alarms train parents to ignore it.
        sos::trigger();
        s_buttonDownAt = 0;
    }
}

void setup() {
    Serial.begin(115200);

    // WiFi config mode: hold SOS button during power-on to enter captive portal.
    // Must run before any WiFi/NimBLE init.
    if (wifi_setup::buttonHeldAtBoot()) {
        wifi_setup::enter();  // blocks for ~5 min, then reboots
    }

    loadNumbers();
    bleInit();
    s_bootMs = millis();

    store::begin();
    sos::begin();
    gps::begin();
    locator::begin();
    motion::begin();
    notify::begin();
    modem::begin();
    report::begin();

    // LED test: flash each color for 1 second at boot
    feedback::begin();
    feedback::ledTest();

    modem::syncClockFromNetwork();
}

// Periodic battery check interval (every 30 minutes)
static uint32_t s_lastBatteryCheck = 0;
static constexpr uint32_t BATTERY_CHECK_INTERVAL_MS = 30UL * 60UL * 1000UL;

void loop() {
    uint32_t now = millis();

    // TEST MODE: send diagnostic SMS every 5 seconds
    if (TEST_MODE) {
        if (now - s_lastTestSms >= TEST_SMS_INTERVAL_MS) {
            s_lastTestSms = now;
            sendTestSms();
        }
        return;  // skip normal operation in test mode
    }

    serviceButton();
    sos::service();
    locator::service();
    motion::service();
    report::service();
    store::drain();
    modem::closeIdle();

    // Periodic battery check (estimated, no ADC)
    if (now - s_lastBatteryCheck >= BATTERY_CHECK_INTERVAL_MS) {
        s_lastBatteryCheck = now;
        checkBattery();
    }

    // SMS command polling — every 10 seconds, check for config texts
    static uint32_t s_lastSmsPoll = 0;
    if (now - s_lastSmsPoll >= 10000) {
        s_lastSmsPoll = now;
        modem::pollSmsCommand(SMS_CMD_SECRET, saveSosNumber, saveScannerNumber);
    }
}
