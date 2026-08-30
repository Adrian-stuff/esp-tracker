#include "net.h"
#include "store.h"
#include "roster.h"
#include "modem.h"
#include "../include/config.h"
#include <stdio.h>
#include <string.h>

namespace net {

bool online() { return modem::attached(); }

size_t drain() {
    if (!store::depth()) return 0;

    // NOT static: these are ~550 bytes together, and drain() runs only
    // once every DRAIN_INTERVAL_MS. Living on the stack for the few
    // hundred ms this function takes costs far less than reserving that
    // space in .bss permanently — see config.h's EEPROM/RAM notes for why
    // every byte here is accounted for.
    Tap batch[BATCH_MAX];
    size_t n = store::peekBatch(batch, BATCH_MAX);
    if (!n) return 0;

    // Hand-built JSON, not ArduinoJson — see JSON_BODY_CAP's comment in
    // config.h for why this has to be a fixed small buffer. If a tap
    // wouldn't fit, stop there and send a smaller batch; the rest stays
    // queued for the next drain.
    char body[JSON_BODY_CAP];
    size_t len = 0;
    len += snprintf(body + len, JSON_BODY_CAP - len, "{\"taps\":[");
    size_t sent = 0;
    for (size_t i = 0; i < n; i++) {
        int wrote = snprintf(body + len, JSON_BODY_CAP - len,
            "%s{\"id\":\"%s\",\"card_uid\":\"%s\",\"recorded_at\":%lu,\"device_sms_sent\":%s}",
            i ? "," : "", batch[i].id, batch[i].uid,
            (unsigned long)batch[i].recorded_at, batch[i].sms_sent ? "true" : "false");
        if (wrote < 0 || len + (size_t)wrote >= JSON_BODY_CAP - 3) break;   // leave room for "]}"
        len += wrote;
        sent++;
    }
    if (!sent) return 0;
    len += snprintf(body + len, JSON_BODY_CAP - len, "]}");

    // Reuses `body` as the response buffer too, rather than a separate
    // resp[]: modem::request() fully sends the request body during CIPSEND
    // before it starts writing anything into the response buffer, so the
    // same memory is safe to serve both roles back to back — see
    // modem.cpp's request(). Saves another ~64 bytes off this function's
    // peak stack.
    int code = modem::request("POST", "/functions/v1/ingest", DEVICE_TOKEN, body, body, sizeof body);

    if (code != 200) return 0;   // stays queued; the next pass retries
    store::commit(sent);         // 200 IS the ack
    return sent;
}

void refreshRosterIfStale() {
    if (!roster::stale()) return;
    modem::requestRosterStream("/functions/v1/roster", DEVICE_TOKEN);
    // Failure leaves the previous cached roster in EEPROM untouched — see
    // modem::requestRosterStream's contract.
}

}
