#include "ble_serial.h"
#include "smsq.h"
#include "../include/config.h"
#include <WiFi.h>
#include <NimBLEDevice.h>

// Nordic UART Service (NUS) — de facto standard for BLE serial.
// Phone apps (nRF Connect, LightBlue, custom apps) can connect and send
// commands through this virtual serial port.

static const char* NUS_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* NUS_RX_CHAR_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* NUS_TX_CHAR_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

static NimBLEServer*         s_server    = nullptr;
static NimBLECharacteristic* s_rxChar    = nullptr;
static NimBLECharacteristic* s_txChar    = nullptr;
static bool                  s_connected = false;

static String s_lineBuf;

class RxCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChar) override {
        NimBLEAttValue val = pChar->getValue();
        for (size_t i = 0; i < val.length(); i++) {
            char c = (char)val[i];
            if (c == '\n' || c == '\r') {
                if (s_lineBuf.length() > 0) {
                    String cmd = s_lineBuf;
                    s_lineBuf = "";

                    if (cmd.startsWith("SMS:")) {
                        String payload = cmd.substring(4);
                        int spaceIdx = payload.indexOf(' ');
                        if (spaceIdx > 0) {
                            String number = payload.substring(0, spaceIdx);
                            String body   = payload.substring(spaceIdx + 1);
                            bool ok = smsq::enqueue(number.c_str(), body.c_str());
                            String resp = ok ? "OK: SMS queued" : "ERR: queue full or SIM900 not ready";
                            ble_serial::notify((resp + "\n").c_str());
                        } else {
                            ble_serial::notify("ERR: SMS:<number> <message>\n");
                        }
                    } else if (cmd == "STATUS") {
                        char buf[128];
                        snprintf(buf, sizeof buf, "WiFi:%s SIM:%s Queue:%u\n",
                                 WiFi.status() == WL_CONNECTED ? "up" : "down",
                                 smsq::ready() ? "ready" : "down",
                                 (unsigned)smsq::depth());
                        ble_serial::notify(buf);
                    } else if (cmd == "PING") {
                        ble_serial::notify("PONG\n");
                    } else {
                        ble_serial::notify("ERR: unknown cmd (SMS:<num> <msg>|STATUS|PING)\n");
                    }
                }
            } else {
                s_lineBuf += c;
                if (s_lineBuf.length() > 256) s_lineBuf = "";
            }
        }
    }
};

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer) override {
        s_connected = true;
        Serial.println("[BLE] client connected");
    }
    void onDisconnect(NimBLEServer* pServer) override {
        s_connected = false;
        Serial.println("[BLE] client disconnected");
        NimBLEDevice::startAdvertising();
    }
};

static RxCallback      s_rxCb;
static ServerCallbacks s_srvCb;

namespace ble_serial {

void begin() {
    NimBLEDevice::init("ESP-Scanner");
    NimBLEDevice::setMTU(512);

    s_server = NimBLEDevice::createServer();
    s_server->setCallbacks(&s_srvCb);

    NimBLEService* svc = s_server->createService(NUS_SERVICE_UUID);

    s_rxChar = svc->createCharacteristic(
        NUS_RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    s_rxChar->setCallbacks(&s_rxCb);

    s_txChar = svc->createCharacteristic(
        NUS_TX_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY
    );

    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SERVICE_UUID);
    adv->setScanResponse(true);
    adv->start();

    Serial.println("[BLE] NUS advertising started");
}

void service() {
    // BLE stack runs in the background; no polling needed.
}

void notify(const char* msg) {
    if (!s_connected || !s_txChar) return;
    s_txChar->setValue((const uint8_t*)msg, strlen(msg));
    s_txChar->notify();
}

}
