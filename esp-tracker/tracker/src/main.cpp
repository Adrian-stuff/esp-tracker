#include <Arduino.h>
#include <sys/time.h>
#include <time.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include "esp_system.h"   // esp_reset_reason() — crash-loop backoff, see setup()
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
#include "battery.h"
#include "wifi_uplink.h"

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

// WiFi uplink credentials — see wifi_uplink.h. Reboot is required to pick
// these up: WiFi.mode()/begin() only run once from setup()'s call to
// wifi_uplink::begin(), matching how the SOS/scanner numbers ALSO only
// take effect this way (loadNumbers() runs once at boot too), not because
// of any new limitation this introduces.
static void saveWifiSsid(const char* ssid) {
    s_prefs.begin("tracker", false);
    s_prefs.putString("wifi_ssid", ssid);
    s_prefs.end();
}

static void saveWifiPass(const char* pass) {
    s_prefs.begin("tracker", false);
    s_prefs.putString("wifi_pass", pass);
    s_prefs.end();
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

// Called by sos.cpp before immediate SMS sends — frees ~8mA.
// Must be safe to call even if BLE was never init'd (e.g. early boot fail).
static void blePowerOff() {
    if (s_bleServer) {
        Serial.println("[BLE] powering off (pre-SMS brownout mitigation)");
        NimBLEDevice::deinit(true);
        s_bleServer = nullptr;
        s_bleTx = nullptr;
        s_bleRx = nullptr;
        s_bleConnected = false;
    }
}

// Parse and execute BLE commands
static void processBleCommand(const char* cmd) {
    // 256, not 128: STATUS now includes modem::lastError() detail (up to 80
    // chars) alongside everything else, and 128 was already tight before
    // that addition. Matches the TX characteristic's own declared capacity
    // (FFF1, created with 256 below) — no benefit to a local buffer bigger
    // than what can actually go out over the wire.
    char buf[256];
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
    } else if (strncmp(cmd, "WIFISSID ", 9) == 0) {
        const char* ssid = cmd + 9;
        saveWifiSsid(ssid);
        snprintf(buf, sizeof buf, "WiFi SSID set to \"%s\" — reboot to connect\n", ssid);
    } else if (strncmp(cmd, "WIFIPASS ", 9) == 0) {
        const char* pass = cmd + 9;
        saveWifiPass(pass);
        snprintf(buf, sizeof buf, "WiFi password set (%u chars) — reboot to connect\n", (unsigned)strlen(pass));
    } else if (strcmp(cmd, "STATUS") == 0) {
        int8_t csq = modem::signalQuality();
        int8_t creg = modem::networkStatus();
        snprintf(buf, sizeof buf,
            "SOS: %s\nScanner: %s\nCSQ: %d\nCREG: %d\nBatt: %u%% (%lumV)\nModem: %s%s%s\nWiFi uplink: %s\n",
            s_sosNumber, s_scannerNumber, csq, creg,
            (unsigned)battery::pct(), (unsigned long)battery::milliVolts(),
            modem::blackout() ? "BLACKOUT SUSPECTED" : "ok",
            modem::lastError()[0] ? ", last error: " : "", modem::lastError(),
            wifi_uplink::connected() ? "connected" : "not connected");
    } else if (strcmp(cmd, "GPS") == 0) {
        // Diagnostic, not a fix request. Indoors a real fix will never
        // arrive — see gps.h's GnssDiag comment — so this reports whether
        // the NEO-6M is actually alive and talking (valid NMEA sentences
        // parsed) rather than waiting for coordinates that can't happen at
        // a desk. Blocks for GPS_TEST_MS to give the module time to say
        // something — same accepted tradeoff as the WIFI command blocking
        // far longer (5 min) for its own portal.
        static constexpr uint32_t GPS_TEST_MS = 5000;
        bool wasActive = sos::active();  // don't fight an in-progress SOS's own acquisition
        Serial.println("[BLE] GPS test requested — powering on NEO-6M, listening 5s...");
        gps::power(true);
        uint32_t start = millis();
        while (millis() - start < GPS_TEST_MS) {
            gps::service();
            delay(10);
        }
        GnssDiag d = gps::diagnostics();
        if (!wasActive) gps::power(false);  // back to the normal power-gated state

        bool alive = d.passedChecksum > 0;
        char sats[8], hdop[12];
        snprintf(sats, sizeof sats, d.satellitesValid ? "%u" : "?", d.satellites);
        if (d.hdopValid) snprintf(hdop, sizeof hdop, "%.1f", d.hdop); else snprintf(hdop, sizeof hdop, "?");

        snprintf(buf, sizeof buf,
            "GPS: %s\nChars:%lu OK:%lu Bad:%lu\nSats used:%s HDOP:%s\nFix:%s\n%s",
            alive ? "MODULE ALIVE (valid NMEA received)" : "NO VALID DATA — check wiring/EN pin/power",
            (unsigned long)d.charsProcessed, (unsigned long)d.passedChecksum, (unsigned long)d.failedChecksum,
            sats, hdop, d.hasFix ? "YES" : "no (normal indoors)",
            (!alive && d.charsProcessed == 0) ? "Nothing on the UART at all — check PIN_GPS_EN/wiring.\n" :
            (!alive) ? "Bytes arriving but none valid — check baud (9600) and RX/TX not swapped.\n" : "");
    } else if (strncmp(cmd, "SEND ", 5) == 0) {
        // Manual SMS test — sends exactly what you type, right now, and
        // reports the real modem result. Useful for checking the SIM800L
        // end-to-end (signal, credit, wiring) without waiting for a
        // scheduled report or physically triggering an SOS.
        const char* rest = cmd + 5;
        const char* sp = strchr(rest, ' ');
        if (!sp || sp == rest || !*(sp + 1)) {
            snprintf(buf, sizeof buf, "Usage: SEND <number> <message>\n");
        } else {
            char num[20];
            size_t numLen = sp - rest;
            if (numLen >= sizeof num) numLen = sizeof num - 1;
            memcpy(num, rest, numLen); num[numLen] = '\0';
            const char* msg = sp + 1;
            Serial.printf("[BLE] SEND requested: to=%s body=\"%s\"\n", num, msg);
            bool ok = modem::sendSms(num, msg);
            if (ok) {
                snprintf(buf, sizeof buf, "Sent OK\n");
            } else {
                snprintf(buf, sizeof buf, "Send FAILED: %s\n", modem::lastError());
            }
        }
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
            "  SEND +639XXXXXXXXX <msg> - send a test SMS right now\n"
            "  GPS                  - test the GNSS module (5s, works indoors)\n"
            "  WIFISSID <ssid>      - set WiFi uplink network (reboot to apply)\n"
            "  WIFIPASS <password>  - set WiFi uplink password (reboot to apply)\n"
            "  STATUS               - show current config\n"
            "  WIFI                 - enter WiFi config mode (captive portal)\n"
            "  HELP                 - this help\n");
    } else {
        snprintf(buf, sizeof buf, "Unknown cmd. Type HELP\n");
    }

    // Send response over BLE (queued, sent when subscribed)
    bleBufAppend(buf, strlen(buf));
    bleFlush();
    Serial.print(buf);
}

// Handoff buffer: BleRxCallback::onWrite() below runs on NimBLE's own host
// task, NOT the Arduino main loop task. modem.cpp and gps.cpp keep
// unsynchronized static state (the AT-command UART exchange, TinyGPSPlus's
// character-by-character parser) that the main loop ALSO touches every
// iteration — routine reports, SOS sends, motion scans. Running a command
// straight from onWrite() would let two FreeRTOS tasks read/write that same
// state at once (e.g. a SEND/GPS/STATUS command colliding with an in-flight
// SOS send or GPS parse), corrupting whichever was mid-exchange. Queuing the
// command here and running it from loop() instead keeps every touch of that
// state on the one task that already owns it everywhere else in this
// firmware — no mutex needed, just "only one task ever calls this".
static volatile bool s_blePending = false;
static char          s_bleCmdBuf[128];

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
            if (len > 0) {
                // A command still mid-processing when another arrives is
                // dropped rather than queued — acceptable for a low-frequency
                // manual debug interface, and simpler than a real queue.
                if (!s_blePending) {
                    strncpy(s_bleCmdBuf, cmd, sizeof(s_bleCmdBuf) - 1);
                    s_bleCmdBuf[sizeof(s_bleCmdBuf) - 1] = '\0';
                    s_blePending = true;
                } else {
                    Serial.println("[BLE] command dropped — previous one still processing");
                }
            }
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

// Real battery reading — see battery.h/.cpp. Replaces a former uptime-based
// guess that never touched PIN_VBAT_SENSE despite pins.h defining it with a
// real divider ratio. Thresholds are config.h's LOWBATT_ALERT_PCT/
// LOWBATT_CRIT_PCT, not a second set of constants duplicated here.
static bool s_batteryWarned = false;
static bool s_batteryCrit   = false;

static void checkBattery() {
    uint8_t pct = battery::pct();
    Serial.printf("[battery] periodic check: %u%%\n", pct);
    if (pct <= LOWBATT_CRIT_PCT && !s_batteryCrit) {
        s_batteryCrit = true;
        Serial.printf("[battery] CRITICAL (<=%u%%) — notifying parent\n", (unsigned)LOWBATT_CRIT_PCT);
        notify::fire(Event::BatteryCritical, nullptr);
        feedback::play(Cue::LowBattery);
    } else if (pct <= LOWBATT_ALERT_PCT && !s_batteryWarned) {
        s_batteryWarned = true;
        Serial.printf("[battery] LOW (<=%u%%) — notifying parent\n", (unsigned)LOWBATT_ALERT_PCT);
        notify::fire(Event::BatteryLow, nullptr);
        feedback::play(Cue::LowBattery);
    }
    // Recovery: a recharge should re-arm both alerts rather than staying
    // permanently latched from before the swap.
    if (pct > LOWBATT_ALERT_PCT && (s_batteryWarned || s_batteryCrit)) {
        Serial.println("[battery] recovered above alert threshold — re-arming alerts");
        s_batteryWarned = false; s_batteryCrit = false;
    }
}

// Interrupt-driven edge capture — NOT a plain digitalRead() poll anymore.
//
// CONFIRMED REAL PROBLEM this exists to fix: loop() can legitimately block
// for many seconds at a stretch (a store::drain() retry attempting to wake
// an unresponsive modem, before the blackout fast-fail above existed —
// and even with it, an occasional long block is still possible). A plain
// poll only ever sees digitalRead() at the instant it happens to run; if
// an ENTIRE press-and-release cycle occurs while loop() is stuck
// elsewhere, the button is never sampled during it at all and the press
// is silently lost — no amount of increasing SOS_HOLD_MS's timeout logic
// fixes that, because the poll never runs to see it.
//
// An interrupt fires on every edge regardless of what loop() is doing, so
// the press and release instants are captured precisely no matter how
// busy the main loop is. serviceButton() below then reconstructs "was
// there a qualifying hold" from those captured timestamps — including
// retroactively, if the hold and release both already happened before
// loop() got back around to checking.
static volatile uint32_t s_isrPressAt   = 0;
static volatile uint32_t s_isrReleaseAt = 0;
static volatile bool     s_isrIsDown    = false;

static void IRAM_ATTR onSosButtonEdge() {
    uint32_t now = millis();
    if (digitalRead(PIN_SOS_BUTTON) == LOW) {
        s_isrPressAt = now;
        s_isrIsDown  = true;
    } else {
        s_isrReleaseAt = now;
        s_isrIsDown    = false;
    }
}

static uint32_t s_lastHandledPressAt  = 0;   // pressAt value already acted on (fired or ruled a tap)
static uint32_t s_lastFeedbackPressAt = 0;   // pressAt we've already given "Pressed" feedback for

static void serviceButton() {
    // 32-bit-aligned volatile reads are atomic on the ESP32 (Xtensa) —
    // no critical section needed for a consistent snapshot of these three.
    uint32_t pressAt   = s_isrPressAt;
    uint32_t releaseAt = s_isrReleaseAt;
    bool     isDown    = s_isrIsDown;

    // Immediate feedback the INSTANT a new press starts — before waiting to
    // see if it becomes a real 2s hold. Previously there was no signal at
    // all until either the full hold completed or nothing happened, which
    // gives no confirmation the button (or the interrupt wiring) is
    // actually working. Tracked separately from s_lastHandledPressAt since
    // this fires immediately on press while that one only resolves once
    // the press's outcome (hold vs. tap) is known.
    if (isDown && pressAt != s_lastFeedbackPressAt) {
        s_lastFeedbackPressAt = pressAt;
        feedback::play(Cue::Pressed);
    }

    if (pressAt == s_lastHandledPressAt) return;   // nothing new since the last press we acted on

    uint32_t heldFor = isDown ? (millis() - pressAt) : (releaseAt - pressAt);

    if (heldFor >= SOS_HOLD_MS) {
        s_lastHandledPressAt = pressAt;   // don't re-fire for the same physical press
        if (sos::active()) {
            // Second 2s hold during the cancel window aborts an accidental press.
            sos::cancel();
        } else {
            // First 2s hold, never a tap: pocket false alarms train parents to ignore it.
            sos::trigger();
        }
    } else if (!isDown) {
        // Released before reaching the hold threshold — a tap, not a hold.
        // Mark handled so we don't keep re-evaluating this same press.
        s_lastHandledPressAt = pressAt;
    }
    // else: still down, not held long enough YET — leave s_lastHandledPressAt
    // alone so the next serviceButton() call re-checks with a fresher millis().
}

// Survives any reset that isn't a full power loss (brownout, SW restart,
// watchdog, panic) — RTC memory, not regular RAM. Counts consecutive
// non-POWERON boots so a crash loop (e.g. the BMS brownout this file's
// other comments reference, repeatedly tripping right after each retry)
// gets a growing recovery delay before the next modem probe, instead of
// hammering a supply that hasn't recovered from the last attempt yet.
// Resets to 0 on a genuine cold power-on or once modem::begin() succeeds.
RTC_DATA_ATTR static uint8_t s_crashLoopCount = 0;
static constexpr uint8_t  CRASH_LOOP_MAX_COUNTED = 10;   // cap the backoff growth
static constexpr uint32_t CRASH_LOOP_DELAY_STEP_MS = 500;

void setup() {
    Serial.begin(115200);
    delay(200);  // let the USB-serial bridge settle before the first print
    Serial.println("\n[boot] tracker starting — " DEVICE_ID);

    esp_reset_reason_t resetReason = esp_reset_reason();
    if (resetReason == ESP_RST_POWERON) {
        s_crashLoopCount = 0;
    } else if (s_crashLoopCount < CRASH_LOOP_MAX_COUNTED) {
        s_crashLoopCount++;
    }
    if (s_crashLoopCount > 0) {
        uint32_t extraDelay = (uint32_t)s_crashLoopCount * CRASH_LOOP_DELAY_STEP_MS;
        Serial.printf("[boot] non-poweron restart #%u in a row — giving the supply %lums extra "
                      "to recover before touching the modem\n",
                      s_crashLoopCount, (unsigned long)extraDelay);
        delay(extraDelay);
    }

    // WiFi config mode: hold SOS button during power-on to enter captive portal.
    // Must run before any WiFi/NimBLE init. buttonHeldAtBoot() itself checks
    // the reset reason again (see its own comment) — a brownout mid-SOS
    // must never boot into a 5-minute WiFi portal with the button dead.
    if (wifi_setup::buttonHeldAtBoot()) {
        Serial.println("[boot] SOS button held at power-on — entering WiFi config portal");
        wifi_setup::enter();  // blocks for ~5 min, then reboots
    }

    loadNumbers();
    Serial.printf("[boot] SOS number: %s, scanner number: %s\n", s_sosNumber, s_scannerNumber);
    bleInit();
    battery::begin();

    store::begin();
    Serial.printf("[boot] store queue depth on boot: %u (survived from before reset, if any)\n", (unsigned)store::depth());
    sos::begin();
    sos::onPowerDown(blePowerOff);
    // Interrupt-driven, not polled — see serviceButton()'s own comment for
    // why: a plain poll can miss an entire press-and-release cycle if
    // loop() is blocked elsewhere when it happens. CHANGE fires on both
    // press and release; onSosButtonEdge() itself just timestamps the edge
    // and returns, all the actual logic stays in serviceButton() on the
    // main loop, not the ISR.
    attachInterrupt(digitalPinToInterrupt(PIN_SOS_BUTTON), onSosButtonEdge, CHANGE);
    gps::begin();
    locator::begin();
    motion::begin();
    notify::begin();
    wifi_uplink::begin();   // presentation/demo backup path — see wifi_uplink.h
    bool modemOk = modem::begin();
    if (!modemOk) {
        Serial.println("[boot] *** MODEM INIT FAILED — SOS/reporting will not work until this recovers ***");
    } else if (s_crashLoopCount > 0) {
        Serial.printf("[boot] modem responded — crash loop broken after %u restart(s)\n", s_crashLoopCount);
        s_crashLoopCount = 0;
    }
    report::begin();

    // LED test: flash each color for 1 second at boot
    feedback::begin();
    feedback::ledTest();

    if (modemOk && modem::syncClockFromNetwork()) {
        Serial.println("[boot] clock synced from network");
    } else {
        Serial.println("[boot] clock sync failed — timestamps will be wrong until this succeeds");
    }
    // From here on the radio idles at CFUN=4 — see config.h's
    // MODEM_CFUN_IDLE_ENABLED block (LiPo BMS brownout mitigation).
    // sendSms() wakes it again on its own whenever something needs sending.
    if (modemOk) modem::enterIdle();
    Serial.println("[boot] setup complete, entering main loop");
}

// Periodic battery check interval (every 30 minutes)
static uint32_t s_lastBatteryCheck = 0;
static constexpr uint32_t BATTERY_CHECK_INTERVAL_MS = 30UL * 60UL * 1000UL;

// USB-serial command console — the same commands as BLE (SEND, STATUS, SOS,
// SCANNER, WIFI, HELP), for testing from a wired connection when there's no
// BLE client handy. Accumulates a line, then hands it to the exact same
// processBleCommand() the BLE RX characteristic uses — one command parser,
// two input paths. Replies go to Serial either way (processBleCommand
// already does that), so nothing extra is needed to see the result here.
static void serviceSerialCommands() {
    static char lineBuf[256];
    static size_t lineLen = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (lineLen > 0) {
                lineBuf[lineLen] = '\0';
                processBleCommand(lineBuf);
                lineLen = 0;
            }
        } else if (lineLen < sizeof(lineBuf) - 1) {
            lineBuf[lineLen++] = c;
        }
    }
}

void loop() {
    uint32_t now = millis();

    // Software watchdog — reset if main loop stalls for 60 seconds
    static uint32_t s_lastLoopRun = 0;
    if (s_lastLoopRun != 0 && (now - s_lastLoopRun) > 60000) {
        Serial.println("[WATCHDOG] loop stalled, resetting");
        delay(100);
        ESP.restart();
    }
    s_lastLoopRun = now;

    // Run any BLE command queued by BleRxCallback::onWrite() — see that
    // handoff buffer's comment for why this can't just run in the callback.
    if (s_blePending) {
        s_blePending = false;
        processBleCommand(s_bleCmdBuf);
    }

    serviceSerialCommands();

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
    wifi_uplink::service();   // presentation/demo backup path — see wifi_uplink.h
    // Skipped while sos.cpp is mid-send: see sos::smsIdle()'s comment — this
    // stops store::drain() from racing sos.cpp's own immediate send and
    // firing an avoidable extra SMS to the scanner before that attempt even
    // gets a chance to run.
    if (sos::smsIdle()) store::drain();
    modem::closeIdle();
    // Non-blocking CFUN=1/CFUN=4 duty-cycle scheduler — see config.h's
    // MODEM_CFUN_IDLE_ENABLED block. Opens periodic windows to check for
    // incoming SMS and drain any store.cpp backlog, then aggressively
    // drops back to CFUN=4 (LiPo BMS brownout mitigation).
    modem::servicePowerCycle();

    // Periodic battery check (estimated, no ADC)
    if (now - s_lastBatteryCheck >= BATTERY_CHECK_INTERVAL_MS) {
        s_lastBatteryCheck = now;
        checkBattery();
    }

    // SMS command polling — only while the radio is actually up (a
    // scheduled window, or sendSms() woke it for an outbound send): AT+CMGL
    // would just fail/waste a cycle while idling at CFUN=4, since a
    // message can only be delivered here while registered in the first
    // place. Polled more often than the old flat 10s while a window IS
    // open, since windows are now the exception rather than the norm.
    static uint32_t s_lastSmsPoll = 0;
    if (modem::radioReady() && now - s_lastSmsPoll >= 3000) {
        s_lastSmsPoll = now;
        modem::pollSmsCommand(SMS_CMD_SECRET, saveSosNumber, saveScannerNumber, sos::onServerAck, report::forceNow);
    }
}
