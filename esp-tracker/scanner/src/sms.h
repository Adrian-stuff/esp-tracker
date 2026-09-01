#pragma once
#include <stdint.h>

// SMS inbox polling — reads incoming SMS from the tracker's location reports
// and relays them to the server over WiFi.
//
// The tracker sends "LOC ..." SMS to this scanner's SIM900 number. This module
// polls AT+CMGL="REC UNREAD" every SMS_POLL_MS, parses the sender and body,
// and posts them to /api/relay/sms over WiFi. The server's tracker_sms.py
// module then decides what to do with the message.

namespace sms {
    void begin();
    void pollInbox();     // call every loop(), non-blocking
}
