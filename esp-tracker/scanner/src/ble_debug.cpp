#include "ble_debug.h"
#include "ble_serial.h"
#include <WiFi.h>
#include <WiFiServer.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

// Telnet debug server — uses existing WiFi, adds ~2KB flash.
// Connect with: nc <scanner-ip> 23
// Or use a telnet app on your phone.

static WiFiServer s_telnet(23);
static WiFiClient s_clients[3];
static int        s_clientCount = 0;

// Simple ring buffer for last 8 messages so new telnet clients see recent history
static char    s_ring[8][128];
static uint8_t s_ringHead = 0;
static uint8_t s_ringCount = 0;

static void ringPush(const char* msg) {
    strncpy(s_ring[s_ringHead], msg, sizeof s_ring[0] - 1);
    s_ring[s_ringHead][sizeof s_ring[0] - 1] = '\0';
    s_ringHead = (s_ringHead + 1) % 8;
    if (s_ringCount < 8) s_ringCount++;
}

static void sendToClients(const char* msg) {
    for (int i = 0; i < s_clientCount; i++) {
        if (s_clients[i].connected()) {
            s_clients[i].print(msg);
        }
    }
}

namespace ble_debug {

void begin() {
    s_telnet.begin();
    s_telnet.setNoDelay(true);
    Serial.println("[TELNET] debug server on port 23");
}

// Call from loop() to accept new clients and handle disconnects
void service() {
    // Accept new clients
    if (s_telnet.hasClient()) {
        WiFiClient newClient = s_telnet.available();
        if (newClient) {
            // Find a free slot or replace oldest
            int slot = -1;
            for (int i = 0; i < s_clientCount; i++) {
                if (!s_clients[i].connected()) { slot = i; break; }
            }
            if (slot == -1 && s_clientCount < 3) {
                slot = s_clientCount++;
            }
            if (slot >= 0) {
                s_clients[slot] = newClient;
                Serial.printf("[TELNET] client connected (slot %d)\n", slot);
                // Send ring buffer history to new client
                for (int j = 0; j < s_ringCount; j++) {
                    int idx = (s_ringHead - s_ringCount + j + 8) % 8;
                    s_clients[slot].print(s_ring[idx]);
                }
                newClient.print("--- connected to ESP-Scanner debug ---\n");
            } else {
                newClient.println("Server full");
                newClient.stop();
            }
        }
    }

    // Clean up disconnected clients
    for (int i = 0; i < s_clientCount; i++) {
        if (s_clients[i] && !s_clients[i].connected()) {
            s_clients[i].stop();
        }
    }

    // Read commands from any connected client
    for (int i = 0; i < s_clientCount; i++) {
        if (s_clients[i] && s_clients[i].connected() && s_clients[i].available()) {
            char cmd[128];
            int len = s_clients[i].readBytesUntil('\n', cmd, sizeof(cmd) - 1);
            cmd[len] = '\0';
            // Trim CR
            while (len > 0 && (cmd[len-1] == '\r' || cmd[len-1] == '\n')) cmd[--len] = '\0';
            if (len > 0) {
                // Handle STATUS command
                if (strcmp(cmd, "STATUS") == 0) {
                    dbg("WiFi: %s | IP: %s | Queue: %u | RTC: %s\n",
                        WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
                        WiFi.localIP().toString().c_str(),
                        (unsigned)0,  // caller should pass real values
                        "see LCD");
                } else if (strcmp(cmd, "HELP") == 0) {
                    dbg("Commands: STATUS, HELP\n");
                } else {
                    dbg("Unknown: %s (type HELP)\n", cmd);
                }
            }
        }
    }
}

void dbg(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    Serial.print(buf);
    ringPush(buf);
    sendToClients(buf);
    ble_serial::notify(buf);
}

}
