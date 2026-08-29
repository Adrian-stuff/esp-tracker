#include "sos.h"
#include "locator.h"
#include "modem.h"
#include "store.h"
#include "feedback.h"
#include "../include/pins.h"
#include "../include/config.h"

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

        // TODO: build the event, push to store, POST it (200 == acked).
        QueuedEvent ev{};
        ev.kind = EventKind::Sos;
        store::push(ev);

        // Parallel dumb channel: no GPRS, no TLS, no server in the path.
        // Whichever arrives first wins; the server de-dupes on event id.
        char sms[160];
        if (have) {
            snprintf(sms, sizeof sms,
                     "SOS from %s. https://maps.google.com/?q=%.5f,%.5f (+/-%dm)",
                     DEVICE_ID, f.lat, f.lon, (int)f.accuracy_m);
        } else {
            snprintf(sms, sizeof sms, "SOS from %s. Position unknown, last known follows.", DEVICE_ID);
        }
        modem::sendSms(SOS_SMS_PRIMARY, sms);

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
