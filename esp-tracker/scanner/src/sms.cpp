#include "sms.h"
#include "smsq.h"
#include "net.h"
#include "../include/config.h"
#include <Arduino.h>

// Polls the SIM900 for incoming SMS (AT+CMGL="REC UNREAD"), extracts sender
// and body, and relays them to the server over WiFi via /api/relay/sms.
// Only runs when the smsq state machine is idle to avoid AT command conflicts.

static uint32_t s_lastPoll = 0;
static String   s_rxBuf;
static enum class St : uint8_t { Idle, WaitList, WaitDelete } s_st = St::Idle;
static uint32_t s_at = 0;

// Parsed SMS fields from the last received message
static char s_sender[20];
static char s_body[202];
static bool s_hasMessage = false;

static void send(const char* cmd) {
    while (smsq::serial().available()) smsq::serial().read();
    s_rxBuf = "";
    smsq::serial().print(cmd);
    smsq::serial().print("\r\n");
}

// Parse +CMGL response lines. Format:
//   +CMGL: <index>,"REC READ","<sender>",,<timestamp>\r\n
//   <body>\r\n
static void parseResponse() {
    s_hasMessage = false;
    int idx = 0;
    while (true) {
        int cmgl = s_rxBuf.indexOf("+CMGL:", idx);
        if (cmgl < 0) break;

        // Extract index number
        int numStart = cmgl + 6;
        while (numStart < (int)s_rxBuf.length() && s_rxBuf[numStart] == ' ') numStart++;
        int numEnd = s_rxBuf.indexOf(',', numStart);
        if (numEnd < 0) break;
        int msgIndex = s_rxBuf.substring(numStart, numEnd).toInt();

        // Extract sender number (third quoted field)
        int firstQuote = s_rxBuf.indexOf('"', numEnd);
        int secondQuote = s_rxBuf.indexOf('"', firstQuote + 1);
        int thirdQuote = s_rxBuf.indexOf('"', secondQuote + 1);
        int fourthQuote = s_rxBuf.indexOf('"', thirdQuote + 1);
        if (fourthQuote < 0) break;

        String sender = s_rxBuf.substring(thirdQuote + 1, fourthQuote);

        // Body is the line after the header
        int headerEnd = s_rxBuf.indexOf("\r\n", fourthQuote);
        if (headerEnd < 0) break;
        int bodyStart = headerEnd + 2;
        int bodyEnd = s_rxBuf.indexOf("\r\n", bodyStart);
        if (bodyEnd < 0) bodyEnd = s_rxBuf.length();
        String body = s_rxBuf.substring(bodyStart, bodyEnd);

        // Only take the first unread message; delete the rest later
        strncpy(s_sender, sender.c_str(), sizeof s_sender - 1);
        s_sender[sizeof s_sender - 1] = 0;
        strncpy(s_body, body.c_str(), sizeof s_body - 1);
        s_body[sizeof s_body - 1] = 0;
        s_hasMessage = true;

        // Queue AT+CMGD for this index
        char cmd[24];
        snprintf(cmd, sizeof cmd, "AT+CMGD=%d", msgIndex);
        send(cmd);
        s_st = St::WaitDelete;
        s_at = millis();
        return;

        // Move past this message for potential future parsing
        idx = bodyEnd;
    }
    // No messages found — go back to idle
    s_st = St::Idle;
}

namespace sms {

void begin() {
    if (!SIM900_PRESENT) return;
    s_st = St::Idle;
}

void pollInbox() {
    if (!SIM900_PRESENT || !smsq::ready()) return;
    if (!net::online()) return;     // no WiFi = nowhere to relay to

    uint32_t now = millis();

    switch (s_st) {
    case St::Idle:
        if (now - s_lastPoll < SMS_POLL_MS) return;
        if (!smsq::isIdle()) return;    // modem busy with outbound SMS
        s_lastPoll = now;
        s_rxBuf = "";
        send("AT+CMGL=\"REC UNREAD\"");
        s_st = St::WaitList;
        s_at = now;
        break;

    case St::WaitList:
        // Wait for OK terminator (indicates AT+CMGL response is complete)
        if (s_rxBuf.indexOf("OK") >= 0) {
            parseResponse();
        } else if (now - s_at > 8000) {
            s_st = St::Idle;    // timeout, try again next cycle
        }
        break;

    case St::WaitDelete:
        if (s_rxBuf.indexOf("OK") >= 0 || now - s_at > 3000) {
            if (s_hasMessage) {
                // Relay to server over WiFi
                net::postRelaySms(s_sender, s_body);
                s_hasMessage = false;
            }
            s_st = St::Idle;
        }
        break;
    }
}

}
