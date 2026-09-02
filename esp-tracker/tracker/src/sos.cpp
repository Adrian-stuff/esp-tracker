#include "sos.h"
#include "locator.h"
#include "modem.h"
#include "store.h"
#include "feedback.h"
#include "../include/pins.h"
#include "../include/config.h"
#include <Arduino.h>
#include <cstdio>

namespace sos {

static bool     s_active      = false;
static uint32_t s_triggered   = 0;
static bool     s_sent        = false;

void begin() {
    pinMode(PIN_SOS_BUTTON, INPUT_PULLUP);
    feedback::begin();
}

bool active() { return s_active; }

void trigger() {
    s_active    = true;
    s_triggered = millis();
    s_sent      = false;

    // 1. Confirm to the child IMMEDIATELY, before anything else.
    //    Non-blocking: this sits on the path racing a 5-second deadline, so it
    //    must not spend 300 ms in delay() the way the motor version did.
    feedback::play(Cue::Armed);

    // 2. Race all four position sources.
    locator::beginAcquire();
}

void service() {
    feedback::service();          // must run even when no SOS is active
    if (!s_active) return;
    locator::service();

    // THE FIVE-SECOND RULE. Do not wait for GNSS: indoors it never returns.
    if (!s_sent && millis() - s_triggered >= SOS_TX_DEADLINE_MS) {
        Fix f{};
        bool have = locator::best(f);

        // Build the event with full location data for the store queue.
        QueuedEvent ev{};
        ev.kind = EventKind::Sos;
        ev.recorded_at = f.recorded_at ? f.recorded_at : millis() / 1000;
        ev.payload_len = 0;
        if (have) {
            ev.payload_len = snprintf(ev.payload, sizeof ev.payload,
                "{\"lat\":%.6f,\"lon\":%.6f,\"accuracy_m\":%.1f,\"source\":\"%s\"}",
                f.lat, f.lon, f.accuracy_m,
                f.source == FixSource::Gnss ? "gnss" :
                f.source == FixSource::Wifi ? "wifi" : "unknown");
        }
        store::push(ev);

        // Parallel dumb channels: no GPRS, no TLS, no server in the path.
        // Whichever arrives first wins; the server de-dupes on event id.
        // SOS goes to BOTH the parent (direct) and the scanner (relay to Supabase).
        char sms[160];
        if (have) {
            snprintf(sms, sizeof sms,
                     "SOS from %s. https://maps.google.com/?q=%.5f,%.5f (+/-%dm)",
                     DEVICE_ID, f.lat, f.lon, (int)f.accuracy_m);
        } else {
            snprintf(sms, sizeof sms, "SOS from %s. Position unknown, last known follows.", DEVICE_ID);
        }
        modem::sendSms(s_sosNumber, sms);         // direct to parent
        if (strlen(s_scannerNumber) > 0 && strcmp(s_scannerNumber, s_sosNumber) != 0) {
            modem::sendSms(s_scannerNumber, sms);  // relay via scanner to Supabase
        }

        feedback::play(Cue::Sent);
        s_sent = true;
    }

    // TODO: keep refining until SOS_REFINE_WINDOW_MS, then high-rate mode.
}

void cancel() { s_active = false; feedback::play(Cue::Cancelled); }

void onServerAck(const char* event_id) {
    store::ack(event_id);
    // "Your parent has been told." With an LED this REPEATS for ~20 s: a cue
    // the child is not looking at is a cue that never arrived.
    feedback::play(Cue::Acked);
}

}
