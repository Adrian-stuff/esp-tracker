#pragma once
#include <stdint.h>
#include "../include/config.h"   // SMS_SEND_TIMEOUT_MS default arg below

// SIM800L — 2G/GSM only.
//
// *** GPRS/HTTP IS NOT PURSUED ON THIS HARDWARE — see modem.cpp's file
// header. *** attach()/postJson()/closeIdle() below are intentionally left
// as stubs, not half-finished: this tracker's SIM800L is 2G-only, and the
// Philippines' NTC-mandated 2G/3G shutdown (complete 2026-12-31) has
// already been confirmed killing GPRS attach on the identical modem
// hardware on the gate scanner (PLAN.md §1b). Routine position now goes
// over SMS instead — see report.h. sleep()/wake() remain because SOS still
// benefits from the modem staying attached-and-sleeping rather than fully
// powered down, independent of whether GPRS itself ever comes up.
//
// TLS was going to run on the ESP32 (SSLClient over TinyGSM's raw socket),
// NOT the modem — do not use AT+HTTPSSL if this is ever revisited, SIM800
// SSL is firmware-dependent and not trustworthy for a child's location
// data. Moot for now: there's no GPRS socket for it to wrap.

namespace modem {
    bool begin();
    bool attach();              // stub — always false, see file header
    bool attached();            // stub — always false

    void sleep();               // AT+CSCLK=1, release DTR
    void wake();                // assert DTR, wait for AT response

    // One-time transition from begin()'s power-on default (CFUN=1) into
    // the CFUN=4 idle baseline. Call once from setup(), after attempting
    // syncClockFromNetwork() (which needs registration — see modem.cpp's
    // begin()) and before entering loop().
    void enterIdle();

    // Non-blocking CFUN=1/CFUN=4 duty-cycle scheduler — see config.h's
    // MODEM_CFUN_IDLE_ENABLED block for the brownout problem this exists
    // to mitigate. Call every loop() iteration. Independent of sendSms()'s
    // own self-contained wake/sleep (see below) — this one is specifically
    // for periodically checking for INCOMING SMS, and for giving a queued
    // OUTBOUND send (store.cpp) a window to drain without each one having
    // to individually pay the registration wait.
    void servicePowerCycle();

    // Request the scheduler open a window NOW instead of waiting for the
    // next periodic MODEM_WAKE_INTERVAL_MS check or store-queue-driven
    // wake. sos::trigger() calls this so an incoming ack/config command
    // isn't stuck waiting minutes for the next window — the SOS's own
    // OUTBOUND send does not depend on this, sendSms() below wakes the
    // radio itself regardless of scheduler state.
    void forceWindow();

    // True only while the radio is actually up and registered right now —
    // mid scheduled window, or because sendSms() is mid-send. False the
    // rest of the time (CFUN=4, radio off).
    bool radioReady();

    // True once several consecutive AT commands have timed out in a row —
    // see modem.cpp's atWait(). A brownout mid-TX-burst (SIM800L draws up
    // to 2A; see AGENTS.md's power warning) is the classic cause. Callers
    // can use this to distinguish "briefly out of coverage" (signalQuality/
    // networkStatus just report that normally) from "the module fell off
    // the bus". Clears itself the moment any AT command succeeds again.
    bool blackout();

    // Human-readable reason the last sendSms() call failed, or "" if the
    // last send succeeded (or none has been attempted yet). Set alongside
    // the [modem] Serial log line for the same failure — use this to show
    // the same detail somewhere that isn't the serial console.
    const char* lastError();

    // Stub — always -1, see file header. No GPRS attach means nothing to
    // POST to; kept in the interface rather than deleted in case a future
    // 3G/4G-capable modem swap ever makes this viable again.
    int  postJson(const char* path, const char* json);
    void closeIdle();           // no-op without attach()

    // TLS validation needs a roughly-correct clock and the ESP32 has no RTC.
    bool syncClockFromNetwork();  // AT+CCLK? after NITZ

    // Direct SMS. Needs no GPRS, no TLS, and no server — it survives network
    // congestion, a misconfigured APN, and your server being down. This is the
    // reason the SIM800L substitution is not purely a downgrade.
    //
    // BLOCKS the caller for up to timeoutMs waiting for the modem's final
    // "OK" (default SMS_SEND_TIMEOUT_MS). sos.cpp's immediate parent/scanner
    // sends pass a shorter SOS_IMMEDIATE_SMS_TIMEOUT_MS explicitly — see
    // config.h — because the whole main loop, including the SOS button's
    // cancel-hold detection, is frozen for the duration of this call, and
    // the default timeout alone can exceed SOS_CANCEL_WINDOW_MS.
    //
    // If the radio is idle (CFUN=4 — see config.h's MODEM_CFUN_IDLE_ENABLED
    // block), this wakes it (CFUN=1), waits up to regTimeoutMs for network
    // registration, sends, then drops straight back to CFUN=4 — ALL inside
    // this one blocking call, so every existing caller keeps working
    // unchanged. sos.cpp passes a short regTimeoutMs explicitly so a cold
    // wake can't blow far past the SOS 5-second budget; everything else
    // uses the longer default, which is fine for routine/non-urgent sends.
    bool sendSms(const char* number, const char* text,
                uint32_t timeoutMs = SMS_SEND_TIMEOUT_MS,
                uint32_t regTimeoutMs = MODEM_REG_TIMEOUT_MS);

    // Cell tower identity — the position source of last resort.
    bool cellInfo(uint16_t& mcc, uint16_t& mnc, uint16_t& lac, uint32_t& cellId, int8_t& rssi);

    int8_t signalQuality();     // AT+CSQ, -1 on failure

    // Network registration status — AT+CREG?
    // Returns: 0=not registered, 1=home, 2=searching, 3=denied, 5=roaming, -1=error
    int8_t networkStatus();

    // SMS command interface — polls for incoming SMS and processes config commands.
    // Format: <secret> <command>  e.g. "changeme SOS +63912345678"
    // Returns true if a command was processed and a reply SMS was sent.
    //
    // onAck fires for "<secret> ACK <local-queue-id>" — sent back by
    // relay-sms via the scanner's existing outbox relay once an SOS this
    // device sent has actually reached the server. See sos::onServerAck()
    // and store.h's file header for what this closes the loop on.
    //
    // onLocate fires for "<secret> LOCATE" — sent by
    // supabase/functions/locate/index.ts (the dashboard's "Locate now"
    // button) via the SMS gateway direct to this device's own number, not
    // through the scanner or the outbox. See report::forceNow().
    bool pollSmsCommand(const char* secret,
                        void (*onSetSos)(const char*),
                        void (*onSetScanner)(const char*),
                        void (*onAck)(const char*) = nullptr,
                        void (*onLocate)() = nullptr);
}
