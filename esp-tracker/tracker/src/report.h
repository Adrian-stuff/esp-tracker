#pragma once

// Routine location reporting over SMS — the tracker's uplink for routine
// position, now that GPRS is confirmed dead on this hardware/carrier (see
// ../include/config.h's SCANNER_SMS_NUMBER comment and PLAN.md §1b's GPRS
// section). Texts the scanner's own SIM800L, which relays it to the server
// — see scanner-uno/sms_scanner/src/modem.h's pollSms() and
// scanner-uno/dashboard/app.py, the only path this data takes from here to
// server/app/tracker_sms.py.
//
// SOS (sos.cpp) is UNRELATED and unaffected: it already sends its own
// direct-to-parent SMS on its own path, no relay, no change here. This
// module is purely the ROUTINE, non-emergency position trail.

namespace report {
    void begin();
    void service();   // call from loop(); sends on the cadence set by motion::state() — see config.h's REPORT_INTERVAL_*_MS
}
