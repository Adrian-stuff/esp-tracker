#include "modem.h"
#include "../include/pins.h"
#include "../include/config.h"
#include <TinyGsmClient.h>
#include <HardwareSerial.h>

static HardwareSerial s_serial(2);
// TODO: TinyGsm + SSLClient + ArduinoHttpClient stack.
//   TinyGsm        modem(s_serial);
//   TinyGsmClient  raw(modem);
//   SSLClient      tls(&raw);        <-- crypto on the ESP32, not the SIM800L
//   HttpClient     http(tls, API_HOST, API_PORT);
// Pin the server cert/CA rather than shipping a full bundle: one endpoint,
// less flash, stronger guarantee.

namespace modem {

bool begin() {
    pinMode(PIN_MODEM_EN, OUTPUT);
    pinMode(PIN_MODEM_PWRKEY, OUTPUT);
    pinMode(PIN_MODEM_DTR, OUTPUT);
    digitalWrite(PIN_MODEM_EN, HIGH);
    // PWRKEY: pulse LOW ~1s to bring the module up.
    digitalWrite(PIN_MODEM_PWRKEY, LOW);  delay(1100);
    digitalWrite(PIN_MODEM_PWRKEY, HIGH);
    s_serial.begin(9600, SERIAL_8N1, PIN_MODEM_RX, PIN_MODEM_TX);
    return true;  // TODO: wait for AT / OK
}

bool attach()        { return false; }  // TODO: waitForNetwork + gprsConnect(APN,...)
bool attached()      { return false; }  // TODO

void sleep() { if (MODEM_USE_CSCLK) { /* TODO: AT+CSCLK=1 */ digitalWrite(PIN_MODEM_DTR, HIGH); } }
void wake()  { digitalWrite(PIN_MODEM_DTR, LOW); delay(50); /* TODO: wait for OK */ }

int  postJson(const char* path, const char* json) { (void)path;(void)json; return -1; } // TODO
void closeIdle()     { }                // TODO
bool syncClockFromNetwork() { return false; }  // TODO: AT+CCLK?  — required before any TLS handshake

bool sendSms(const char* number, const char* text) { (void)number;(void)text; return false; } // TODO: AT+CMGF=1, AT+CMGS

bool cellInfo(uint16_t& mcc, uint16_t& mnc, uint16_t& lac, uint32_t& cellId, int8_t& rssi) {
    (void)mcc;(void)mnc;(void)lac;(void)cellId;(void)rssi; return false;  // TODO: AT+CENG=1,1
}

int8_t signalQuality() { return -1; }   // TODO: AT+CSQ

}
