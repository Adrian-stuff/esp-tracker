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
static uint8_t  s_smsStep     = 0;  // 0= idle, 1= sending parent, 2= sending scanner, 3= done

void begin() {
    pinMode(PIN_SOS_BUTTON, INPUT_PULLUP);
    feedback::begin();
}

bool active() { return s_active; }

void trigger() {
    s_active    = true;
    s_triggered = millis();
    s_sent      = false;
    s_smsStep   = 0;

    // 1. Confirm to the child IMMEDIATELY, before anything else.
    feedback::play(Cue::Armed);

    // 2. Race all four position sources.
    locator::beginAcquire();
}

// Non-blocking SMS state machine — called every loop iteration.
// Sends one SMS per call, returns true while still working.
static bool serviceSms() {
    if (s_smsStep == 0) return false;  // nothing to send

    if (s_smsStep == 1) {
        // First attempt: send to parent
        Fix f{};
        bool have = locator::best(f);

        char sms[160];
        if (have) {
            snprintf(sms, sizeof sms,
                     "SOS from %s. https://maps.google.com/?q=%.5f,%.5f (+/-%dm)",
                     DEVICE_ID, f.lat, f.lon, (int)f.accuracy_m);
        } else {
            snprintf(sms, sizeof sms, "SOS from %s. Position unknown, last known follows.", DEVICE_ID);
        }

        // Try to send — if modem is busy, skip and move on
        if (modem::sendSms(s_sosNumber, sms)) {
            s_smsStep = 2;  // success, try scanner next
        } else {
            s_smsStep = 2;  // failed, try scanner anyway
        }
        return true;
    }

    if (s_smsStep == 2) {
        // Second attempt: relay via scanner
        if (strlen(s_scannerNumber) > 0 && strcmp(s_scannerNumber, s_sosNumber) != 0) {
            Fix f{};
            bool have = locator::best(f);
            char sms[160];
            if (have) {
                snprintf(sms, sizeof sms,
                         "SOS from %s. https://maps.google.com/?q=%.5f,%.5f (+/-%dm)",
                         DEVICE_ID, f.lat, f.lon, (int)f.accuracy_m);
            } else {
                snprintf(sms, sizeof sms, "SOS from %s. Position unknown, last known follows.", DEVICE_ID);
            }
            modem::sendSms(s_scannerNumber, sms);
        }
        s_smsStep = 3;  // done
        s_sent = true;
        feedback::play(Cue::Sent);
        return false;
    }

    return false;
}

void service() {
    feedback::service();          // must run even when no SOS is active
    if (!s_active) return;
    locator::service();

    // Non-blocking SMS sending
    if (s_smsStep > 0) {
        serviceSms();
        return;  // don't check timeouts while sending
    }

    // THE FIVE-SECOND RULE. Do not wait for GNSS: indoors it never returns.
    if (!s_sent && millis() - s_triggered >= SOS_TX_DEADLINE_MS) {
        // Build the event with full location data for the store queue.
        Fix f{};
        bool have = locator::best(f);

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

        // Start non-blocking SMS sending
        s_smsStep = 1;
    }

    // Auto-reset SOS after refine window — allows new SOS triggers
    if (s_sent && millis() - s_triggered >= SOS_REFINE_WINDOW_MS) {
        s_active = false;
    }
}

void cancel() { s_active = false; feedback::play(Cue::Cancelled); }

void onServerAck(const char* event_id) {
    store::ack(event_id);
    // "Your parent has been told." With an LED this REPEATS for ~20 s: a cue
    // the child is not looking at is a cue that never arrived.
    feedback::play(Cue::Acked);
}

}
