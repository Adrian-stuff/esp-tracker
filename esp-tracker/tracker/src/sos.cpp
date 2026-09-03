#include "sos.h"
#include "locator.h"
#include "modem.h"
#include "store.h"
#include "feedback.h"
#include "wifi_uplink.h"
#include "gps.h"
#include "../include/pins.h"
#include "../include/config.h"
#include <Arduino.h>
#include <cstdio>

namespace sos {

static bool     s_active      = false;
static uint32_t s_triggered   = 0;
static bool     s_sent        = false;
static uint8_t  s_smsStep     = 0;  // 0= idle, 1= sending parent, 2= sending scanner, 3= done
static char     s_queuedId[24] = "";  // store::push()'d retry copy for this SOS, if any
static sos::PowerDownFn s_powerDownFn = nullptr;

void begin() {
    pinMode(PIN_SOS_BUTTON, INPUT_PULLUP);
    feedback::begin();
}

void onPowerDown(PowerDownFn fn) { s_powerDownFn = fn; }

bool active() { return s_active; }
bool smsIdle() { return s_smsStep == 0 || s_smsStep == 3; }

void trigger() {
    Serial.println("[sos] TRIGGERED — 2s hold detected, arming");
    s_active    = true;
    s_triggered = millis();
    s_sent      = false;
    s_smsStep   = 0;
    s_queuedId[0] = '\0';

    // 1. Confirm to the child IMMEDIATELY, before anything else.
    feedback::play(Cue::Armed);

    // 2. Race all four position sources.
    locator::beginAcquire();

    // 3. Ask the modem's power-cycle scheduler for a window too — mostly
    // for the INCOMING side (a real ack, see modem.h). The OUTBOUND sends
    // below don't depend on this: sendSms() wakes the radio itself
    // regardless of scheduler state, with its own short SOS-appropriate
    // registration timeout — see SOS_IMMEDIATE_SMS_TIMEOUT_MS.
    modem::forceWindow();
}

// Non-blocking SMS state machine — called every loop iteration.
// Sends one SMS per call, returns true while still working.
static bool serviceSms() {
    if (s_smsStep == 0) return false;  // nothing to send

    if (s_smsStep == 1) {
        // First attempt: send to parent
        Fix f{};
        bool have = locator::best(f);
        Serial.printf("[sos] t+%lums: sending direct alert to parent (%s), fix=%s\n",
                      (unsigned long)(millis() - s_triggered), s_sosNumber, have ? "yes" : "NONE YET");

        char sms[160];
        if (have) {
            snprintf(sms, sizeof sms,
                     "SOS from %s. https://maps.google.com/?q=%.5f,%.5f (+/-%dm)",
                     DEVICE_ID, f.lat, f.lon, (int)f.accuracy_m);
        } else {
            snprintf(sms, sizeof sms, "SOS from %s. Position unknown, last known follows.", DEVICE_ID);
        }

        // Try to send — if modem is busy, skip and move on. Shorter timeout
        // than the default: see SOS_IMMEDIATE_SMS_TIMEOUT_MS in config.h —
        // this call blocks the whole loop, including button-cancel detection.
        bool ok = modem::sendSms(s_sosNumber, sms, SOS_IMMEDIATE_SMS_TIMEOUT_MS, SOS_REG_TIMEOUT_MS);
        Serial.printf("[sos] parent alert %s\n", ok ? "delivered to modem OK" : "FAILED — moving to scanner relay anyway");
        s_smsStep = 2;  // either way, try scanner next
        return true;
    }

    if (s_smsStep == 2) {
        // Second attempt: relay via scanner — this is the copy that reaches
        // the server (Supabase, escalation ladder, dashboard). The
        // store::push()'d queue entry from service() below stays live
        // regardless of whether this immediate send succeeds: only a REAL
        // ack from the server (sos::onServerAck(), via modem::pollSmsCommand)
        // removes it — see that send call's own comment just below for why.
        if (strlen(s_scannerNumber) > 0 && strcmp(s_scannerNumber, s_sosNumber) != 0) {
            Fix f{};
            bool have = locator::best(f);
            Serial.printf("[sos] t+%lums: relaying to scanner/server (%s), fix=%s, id=%s\n",
                          (unsigned long)(millis() - s_triggered), s_scannerNumber,
                          have ? "yes" : "NONE YET", s_queuedId[0] ? s_queuedId : "(none)");
            char sms[160];
            // " ID:<id>" trails the human-readable text — the PARENT never
            // sees this (that copy, above, is untouched). This copy is
            // relayed machine-to-machine (scanner -> relay-sms), which
            // already tolerates and ignores trailing content after the
            // pattern it matches (see relay-sms's parseSos regex). It's how
            // relay-sms both dedupes a retried SOS against the same button
            // press (instead of re-triggering a second escalation ladder)
            // and knows what id to echo back in an ACK — see modem.h.
            if (have) {
                snprintf(sms, sizeof sms,
                         "SOS from %s. https://maps.google.com/?q=%.5f,%.5f (+/-%dm)%s%s",
                         DEVICE_ID, f.lat, f.lon, (int)f.accuracy_m,
                         s_queuedId[0] ? " ID:" : "", s_queuedId);
            } else {
                snprintf(sms, sizeof sms, "SOS from %s. Position unknown, last known follows.%s%s",
                         DEVICE_ID, s_queuedId[0] ? " ID:" : "", s_queuedId);
            }
            // Same shorter timeout as the parent send above, same reason.
            //
            // Deliberately NOT acking s_queuedId just because this send
            // succeeded: "succeeded" here only means the LOCAL MODEM
            // accepted it, not that it reached the server (same distinction
            // store.h's file header draws elsewhere). The queued copy now
            // stays live until a REAL ack comes back — see
            // modem::pollSmsCommand's onAck / sos::onServerAck() — so it
            // still gets retried by store::drain() if this copy is lost in
            // transit. A retry is safe to send even after this one
            // succeeds: relay-sms dedupes on this same ID server-side, so a
            // redundant resend can't double-trigger the escalation ladder.
            bool ok = modem::sendSms(s_scannerNumber, sms, SOS_IMMEDIATE_SMS_TIMEOUT_MS, SOS_REG_TIMEOUT_MS);
            Serial.printf("[sos] scanner relay %s (queue entry %s)\n",
                          ok ? "delivered to modem OK" : "FAILED",
                          ok ? "still held pending a real server ack" : "held, will retry via store::drain()");
        } else {
            Serial.println("[sos] no distinct scanner number configured — skipping relay");
        }
        s_smsStep = 3;  // done
        s_sent = true;
        Serial.printf("[sos] t+%lums: immediate send attempts complete\n", (unsigned long)(millis() - s_triggered));
        feedback::play(Cue::Sent);

        // Restore WiFi STA so motion.cpp can resume tierWifi() scanning.
        // serviceConnection() will reconnect at its own backoff pace — we
        // don't wait for that here. The5s refine window below gives WiFi
        // time to reconnect and push any pending location data before the
        // SOS window closes.
        wifi_uplink::restore();

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
        // Persistent safety net FIRST: if the device loses power before the
        // immediate sends below finish, this copy survives reboot and still
        // gets retried. push() writes the generated id into ev.id, which we
        // keep so a successful immediate scanner-send (serviceSms() step 2)
        // can cancel this copy instead of both delivering independently.
        store::push(ev);
        strncpy(s_queuedId, ev.id, sizeof(s_queuedId) - 1);
        s_queuedId[sizeof(s_queuedId) - 1] = '\0';
        Serial.printf("[sos] t+%lums: 5s deadline reached, fix=%s — queued as %s, starting immediate sends\n",
                      (unsigned long)(millis() - s_triggered), have ? "yes" : "NONE (position unknown)", s_queuedId);

        // Brownout mitigation: kill WiFi (~100mA), GPS (~45mA), and BLE
        // (~8mA) BEFORE the SIM800L's 2A TX burst. The bulk cap needs every
        // mA of headroom it can get — see config.h's MODEM_CFUN_IDLE_ENABLED
        // block. This runs ONCE at the5s deadline, not per-SMS: the cap gets
        // the full modem wake + registration gap to recover before TX.
        wifi_uplink::off();
        gps::off();
        if (s_powerDownFn) s_powerDownFn();
        Serial.printf("[sos] peripherals powered down for SMS TX (pre-brownout mitigation)\n");

        // Start non-blocking SMS sending
        s_smsStep = 1;
    }

    // Auto-reset SOS after refine window — allows new SOS triggers
    if (s_sent && millis() - s_triggered >= SOS_REFINE_WINDOW_MS) {
        Serial.println("[sos] refine window elapsed, resetting — ready for a new trigger");
        s_active = false;
    }
}

void cancel() {
    Serial.println("[sos] CANCELLED — second hold detected inside the cancel window");
    s_active = false;
    feedback::play(Cue::Cancelled);
}

void onServerAck(const char* event_id) {
    Serial.printf("[sos] real server ack received for %s — clearing queue entry\n", event_id);
    store::ack(event_id);
    // "Your parent has been told." With an LED this REPEATS for ~20 s: a cue
    // the child is not looking at is a cue that never arrived.
    feedback::play(Cue::Acked);
}

}
