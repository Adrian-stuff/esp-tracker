#include "wifi_setup.h"
#include "../include/pins.h"
#include "../include/config.h"
#include "modem.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "esp_system.h"   // esp_reset_reason() — see buttonHeldAtBoot()'s safety guard

// AP credentials — open network, no password (config is ephemeral)
static const char* AP_SSID = "Tracker-Setup";

// Captive portal serves on 192.168.4.1 (ESP32 AP default)
static IPAddress AP_IP(192, 168, 4, 1);
static IPAddress AP_GATEWAY(192, 168, 4, 1);
static IPAddress AP_SUBNET(255, 255, 255, 0);

static WebServer server(80);
static DNSServer dns;
static Preferences prefs;

// 5 minutes — long enough to fill in the form, short enough to not kill battery
static constexpr uint32_t SETUP_TIMEOUT_MS = 5UL * 60UL * 1000UL;

// ---- RGB LED helpers (common anode, active LOW) ----
static void setupColor(uint8_t r, uint8_t g, uint8_t b) {
    analogWrite(PIN_LED_R, 255 - r);
    analogWrite(PIN_LED_G, 255 - g);
    analogWrite(PIN_LED_B, 255 - b);
}

static void setupBlink(int count) {
    for (int i = 0; i < count; i++) {
        setupColor(0, 0, 255);   // blue
        delay(150);
        setupColor(0, 0, 0);
        delay(150);
    }
}

// ---- HTML config page ----
// Inline to avoid SPIFFS dependency. Dark mode aware, mobile-first.
static const char* CONFIG_PAGE = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Tracker Setup</title>
<style>
*{box-sizing:border-box}
body{
  margin:0; padding:20px; font:15px/1.5 system-ui,-apple-system,sans-serif;
  background:#eef1f5; color:#141a21;
}
@media(prefers-color-scheme:dark){
  body{ background:#0e1318; color:#e5ebf2 }
  .card{ background:#161d25; border-color:#2b3641; color:#e5ebf2 }
  .sub,.hint,.status{ color:#a7b3c1 }
  input{ background:#161d25; color:#e5ebf2; border-color:#2b3641 }
  .info{ background:#0d2137; border-color:#1a4a6e; color:#e5ebf2 }
  .info .code{ background:rgba(255,255,255,.1) }
  .status-box{ background:#1a2530; border-color:#2b3641; color:#e5ebf2 }
  .status-box h2,.status-box p{ color:#e5ebf2 }
}
.card{
  max-width:420px; margin:0 auto; padding:24px; border-radius:8px;
  border:1px solid #cfd8e2; background:#fff; color:#141a21;
}
h1{ margin:0 0 4px; font-size:20px; color:#141a21 }
.sub{ color:#6d7b8b; margin:0 0 20px; font-size:13px }
label{ display:block; margin:12px 0 4px; font-size:13px; font-weight:600 }
input{
  width:100%; padding:10px 12px; border:1px solid #cfd8e2; border-radius:4px;
  font:inherit; background:#eef1f5; color:#141a21;
}
.hint{ font-size:12px; color:#6d7b8b; margin:4px 0 0 }
.btn{
  display:block; width:100%; margin-top:20px; padding:12px; border:0;
  border-radius:4px; background:#0b6e68; color:#fff; font:inherit;
  font-weight:600; cursor:pointer;
}
.btn:hover{ background:#095c57 }
.led{
  display:inline-block; width:10px; height:10px; border-radius:50%;
  background:#46c3b3; margin-right:6px; vertical-align:middle;
}
.status{ font-size:12px; color:#6d7b8b; margin-top:12px; text-align:center }
.status-box{
  margin-top:16px; padding:12px 16px; border-radius:6px;
  background:#f5f5f5; border:1px solid #e0e0e0; color:#141a21;
}
.status-box h2{ margin:0 0 8px; font-size:13px; color:#141a21 }
.status-box p{ margin:4px 0; font-size:13px; color:#141a21 }
.status-box .ok{ color:#2c6e4e; font-weight:600 }
.status-box .fail{ color:#b93630; font-weight:600 }
.info{
  margin-top:20px; padding:16px; border-radius:6px;
  background:#e3f2fd; border:1px solid #90caf9; color:#141a21;
}
.info h2{ margin:0 0 10px; font-size:14px; color:#141a21 }
.info p{ margin:6px 0; font-size:13px; line-height:1.5; color:#141a21 }
.info .code{
  display:inline-block; padding:3px 8px; border-radius:3px;
  background:rgba(0,0,0,.08); font-family:ui-monospace,monospace;
  font-weight:600; font-size:14px; letter-spacing:.02em;
}
.info a{ color:#0b6e68; font-weight:600 }
</style>
</head>
<body>
<div class="card">
  <h1>Tracker Setup</h1>
  <p class="sub">Configure your child's tracker. Changes save immediately.</p>
  <form action="/save" method="POST">
    <label for="sos">SOS phone number</label>
    <input id="sos" name="sos" type="tel" placeholder="+639123456789" maxlength="20" required>
    <p class="hint">Where SOS alerts are sent. Include country code.</p>

    <label for="scanner">Scanner phone number</label>
    <input id="scanner" name="scanner" type="tel" placeholder="+639325762230" maxlength="20" required>
    <p class="hint">The gate scanner's SIM number. Position reports go here.</p>

    <label for="name">Child's name</label>
    <input id="name" name="name" type="text" placeholder="Ana" maxlength="30">

    <label for="wssid">WiFi uplink network (optional, backup path)</label>
    <input id="wssid" name="wssid" type="text" placeholder="Presentation-WiFi" maxlength="32">
    <p class="hint">Direct-to-server backup over WiFi, bypassing the scanner — see the dashboard
    setup guide's "WiFi uplink" section. Leave blank to stay SMS-only.</p>

    <label for="wpass">WiFi password</label>
    <input id="wpass" name="wpass" type="password" placeholder="(leave blank if unchanged)" maxlength="64">

    <button class="btn" type="submit">Save &amp; reboot</button>
  </form>

  <div class="status-box">
    <h2>SIM800L status</h2>
    <p><strong>Signal:</strong> __CSQ__</p>
    <p><strong>Network:</strong> __CREG__</p>
  </div>

  <div class="info">
    <h2>Connect to dashboard</h2>
    <p><strong>Device ID:</strong> <span class="code">__DEVICE_ID__</span></p>
    <p><strong>Claim code:</strong> <span class="code">__CLAIM_CODE__</span></p>
    <p>1. Open the dashboard: <a href="__DASHBOARD_URL__">__DASHBOARD_URL_SHORT__</a></p>
    <p>2. Sign up with your email</p>
    <p>3. Enter the claim code above to pair this tracker</p>
  </div>

  <p class="status"><span class="led"></span>Config portal active — timeout in ~5 min</p>
</div>
</body>
</html>
)rawliteral";

static const char* SAVED_PAGE = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Saved</title>
<style>
body{
  margin:0; padding:40px 20px; font:15px/1.5 system-ui,sans-serif;
  background:#eef1f5; color:#141a21; text-align:center;
}
@media(prefers-color-scheme:dark){ body{ background:#0e1318; color:#e5ebf2 } }
.check{ font-size:48px; margin-bottom:12px }
h1{ font-size:18px; margin:0 0 8px }
p{ color:#6d7b8b; font-size:14px }
</style>
</head>
<body>
<div class="check">&#10003;</div>
<h1>Saved!</h1>
<p>Tracker will reboot in a few seconds.<br>
Connect to the tracker's WiFi again to verify settings.</p>
</body>
</html>
)rawliteral";

// ---- form handling ----

static void handleRoot() {
    // Pre-fill form with current values
    String page = CONFIG_PAGE;

    String sosVal, scannerVal, nameVal, wssidVal;
    prefs.begin("tracker", true);
    sosVal = prefs.getString("sos_number", SOS_SMS_PRIMARY);
    scannerVal = prefs.getString("scanner_number", SCANNER_SMS_NUMBER);
    nameVal = prefs.getString("child_name", CHILD_NAME);
    wssidVal = prefs.getString("wifi_ssid", "");
    prefs.end();
    if (wssidVal == "change-me") wssidVal = "";   // compiled-in placeholder, not a real saved value

    // Replace placeholder values in the HTML
    page.replace("placeholder=\"+639123456789\"", String("value=\"") + sosVal + "\" placeholder=\"+639123456789\"");
    page.replace("placeholder=\"+639325762230\"", String("value=\"") + scannerVal + "\" placeholder=\"+639325762230\"");
    page.replace("placeholder=\"Ana\"", String("value=\"") + nameVal + "\" placeholder=\"Ana\"");
    // Password is NEVER pre-filled/echoed back, same reason browsers don't:
    // whoever opens this portal next (possibly a different person) sees a
    // blank field, not the previously-saved secret.
    if (wssidVal.length()) {
        page.replace("placeholder=\"Presentation-WiFi\"",
                     String("value=\"") + wssidVal + "\" placeholder=\"Presentation-WiFi\"");
    }

    // Inject device info for parent dashboard pairing
    page.replace("__DEVICE_ID__", DEVICE_ID);
    page.replace("__CLAIM_CODE__", CLAIM_CODE);

    // Dashboard URL — where parents go to pair
    const char* dashUrl = DASHBOARD_URL;
    page.replace("__DASHBOARD_URL__", dashUrl);
    // Short display version (strip https://)
    String shortUrl = dashUrl;
    if (shortUrl.startsWith("https://")) shortUrl.remove(0, 8);
    else if (shortUrl.startsWith("http://")) shortUrl.remove(0, 7);
    page.replace("__DASHBOARD_URL_SHORT__", shortUrl);

    // SIM800L status
    int8_t csq = modem::signalQuality();
    int8_t creg = modem::networkStatus();
    String csqStr = (csq > 0) ? String(csq) + "/31" : "No signal";
    String cregStr;
    switch (creg) {
        case 1: cregStr = "Registered (home)"; break;
        case 5: cregStr = "Registered (roaming)"; break;
        default: cregStr = "Not registered"; break;
    }
    page.replace("__CSQ__", csqStr);
    page.replace("__CREG__", cregStr);

    server.send(200, "text/html", page);
}

static void handleSave() {
    String sos = server.arg("sos");
    String scanner = server.arg("scanner");
    String name = server.arg("name");
    String wssid = server.arg("wssid");
    String wpass = server.arg("wpass");

    prefs.begin("tracker", false);
    if (sos.length() >= 10) prefs.putString("sos_number", sos);
    if (scanner.length() >= 10) prefs.putString("scanner_number", scanner);
    if (name.length() > 0) prefs.putString("child_name", name);
    // WiFi uplink is optional (backup path — see wifi_uplink.h): an empty
    // SSID field is a deliberate "stay SMS-only", not a mistake, so it's
    // saved as-is rather than skipped like the required fields above.
    prefs.putString("wifi_ssid", wssid);
    if (wpass.length() > 0) prefs.putString("wifi_pass", wpass);  // blank = "leave unchanged"
    prefs.end();

    server.send(200, "text/html", SAVED_PAGE);

    // Brief LED flash to confirm, then reboot
    delay(500);
    setupColor(0, 255, 0);  // green flash
    delay(300);
    ESP.restart();
}

// Captive portal: redirect ALL requests to the config page
// This triggers Android/iOS captive portal detection automatically.
static void handleNotFound() {
    server.sendHeader("Location", String("http://") + AP_IP.toString(), true);
    server.send(302, "text/plain", "");
}

static const char* resetReasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW (ESP.restart())";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

bool wifi_setup::buttonHeldAtBoot() {
    // SAFETY GUARD — only ever enter setup mode on a genuine cold power-on.
    //
    // A SIM800L TX burst browning out the ESP32 mid-send is a CONFIRMED
    // real failure mode on this hardware (see AGENTS.md's power warning,
    // config.h's MODEM_CFUN_IDLE_ENABLED block). If that happens WHILE a
    // child is holding the SOS button — the exact moment it's most likely,
    // since triggering an SOS is what starts the SMS send in the first
    // place — the device reboots with the button STILL physically held.
    // Without this guard, that reads identically to "someone is
    // deliberately holding SOS at power-on to enter setup" and the device
    // boots into a 5-minute WiFi captive portal with the SOS button
    // completely unresponsive — during an actual emergency, that is the
    // worst possible failure this firmware could produce.
    //
    // A deliberate "hold SOS at boot" gesture only ever happens paired
    // with an actual power cycle (unplugging/replugging, a fresh battery
    // connection) — exactly what ESP_RST_POWERON means. Every other reset
    // reason (brownout, software restart, watchdog, panic) skips this
    // check entirely, regardless of button state, and falls through to
    // normal boot — where store.cpp's persisted queue (see store.h) and
    // serviceButton()'s own hold-detection naturally pick the SOS back up:
    // if the button is still held once loop() starts, a fresh 2s hold is
    // detected and sos::trigger() fires again on its own.
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason != ESP_RST_POWERON) {
        Serial.printf("[wifi_setup] reset reason: %s (not a cold power-on) — "
                      "skipping WiFi setup check, button state ignored this boot\n",
                      resetReasonName(reason));
        return false;
    }

    pinMode(PIN_SOS_BUTTON, INPUT_PULLUP);
    delay(50);  // let pull-up settle
    return digitalRead(PIN_SOS_BUTTON) == LOW;
}

void wifi_setup::enter() {
    // Configure LED pins early for feedback
    pinMode(PIN_LED_R, OUTPUT);
    pinMode(PIN_LED_G, OUTPUT);
    pinMode(PIN_LED_B, OUTPUT);

    // Blue blink pattern: 3 quick flashes = "entering setup mode"
    setupBlink(3);

    // Init modem briefly so SIM800L status is available in the portal
    modem::begin();

    // Start AP — open network, config-only
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID);
    delay(200);  // let AP stabilize
    WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);

    // DNS: redirect everything to the AP IP (captive portal trigger)
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dns.start(53, "*", AP_IP);

    // Web server routes
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.onNotFound(handleNotFound);
    server.begin();

    // Slow blue pulse = "config portal active"
    setupColor(0, 0, 40);

    uint32_t start = millis();
    while (millis() - start < SETUP_TIMEOUT_MS) {
        dns.processNextRequest();
        server.handleClient();
        delay(10);  // yield to WiFi stack

        // Slow blue pulse feedback
        uint32_t elapsed = (millis() - start) / 1000;
        if (elapsed % 2 == 0) {
            setupColor(0, 0, 40);
        } else {
            setupColor(0, 0, 0);
        }
    }

    // Timeout — reboot
    server.stop();
    dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    delay(200);
    ESP.restart();
}
