#include "wifi_setup.h"
#include "../include/pins.h"
#include "../include/config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

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
  input{ background:#161d25; color:#e5ebf2; border-color:#2b3641 }
  .card{ background:#161d25; border-color:#2b3641 }
}
.card{
  max-width:400px; margin:0 auto; padding:24px; border-radius:8px;
  border:1px solid #cfd8e2; background:#fff;
}
h1{ margin:0 0 4px; font-size:20px }
.sub{ color:#6d7b8b; margin:0 0 20px; font-size:13px }
label{ display:block; margin:12px 0 4px; font-size:13px; font-weight:600 }
input{
  width:100%; padding:10px 12px; border:1px solid #cfd8e2; border-radius:4px;
  font:inherit; background:#eef1f5;
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

    <button class="btn" type="submit">Save &amp; reboot</button>
  </form>
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

    String sosVal, scannerVal, nameVal;
    prefs.begin("tracker", true);
    sosVal = prefs.getString("sos_number", SOS_SMS_PRIMARY);
    scannerVal = prefs.getString("scanner_number", SCANNER_SMS_NUMBER);
    nameVal = prefs.getString("child_name", CHILD_NAME);
    prefs.end();

    // Replace placeholder values in the HTML
    page.replace("placeholder=\"+639123456789\"", String("value=\"") + sosVal + "\" placeholder=\"+639123456789\"");
    page.replace("placeholder=\"+639325762230\"", String("value=\"") + scannerVal + "\" placeholder=\"+639325762230\"");
    page.replace("placeholder=\"Ana\"", String("value=\"") + nameVal + "\" placeholder=\"Ana\"");

    server.send(200, "text/html", page);
}

static void handleSave() {
    String sos = server.arg("sos");
    String scanner = server.arg("scanner");
    String name = server.arg("name");

    prefs.begin("tracker", false);
    if (sos.length() >= 10) prefs.putString("sos_number", sos);
    if (scanner.length() >= 10) prefs.putString("scanner_number", scanner);
    if (name.length() > 0) prefs.putString("child_name", name);
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

bool wifi_setup::buttonHeldAtBoot() {
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
