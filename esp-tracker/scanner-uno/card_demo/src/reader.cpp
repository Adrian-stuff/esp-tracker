#include "reader.h"
#include "clock.h"
#include "../include/pins.h"
#include "../include/config.h"
#include <SPI.h>
#include <MFRC522.h>

static MFRC522  s_rfid(PIN_RFID_SS, PIN_RFID_RST);
static char     s_lastUid[24] = {0};
static uint32_t s_lastAt      = 0;
static bool     s_dup         = false;
static bool     s_dupAnnounced = false;  // true once this debounce window's duplicate has been reported
static bool     s_present     = false;   // true if the previous poll() saw this same card still there
static uint32_t s_seq         = 0;

namespace reader {

void begin() {
    SPI.begin();                  // Uno SPI pins are fixed (11/12/13), not passed here
    s_rfid.PCD_Init();
    delay(50);                    // some RC522 clones need settling time after reset
    // Verify SPI — a dead register means wiring fault or 5V damage.
    byte v = s_rfid.PCD_ReadRegister(MFRC522::VersionReg);
    if (v == 0x00 || v == 0xFF) {
        Serial.print(F("RC522 DEAD ver=0x"));
        Serial.println(v, HEX);
    }
    s_rfid.PCD_AntennaOn();
}

bool sawDuplicate() { return s_dup; }

bool poll(Tap& out) {
    s_dup = false;

    // Try REQA first (fast path — covers the common case of a fresh tap
    // on a card that was physically lifted since the last release()).  Fall
    // back to WUPA for cards that are still sitting on the reader from the
    // previous tap (HALTed by release()).  Cheap RC522 clones sometimes
    // have issues with one or the other, so trying both is more robust —
    // confirmed by the card_writer's identical fix.
    bool detected = s_rfid.PICC_IsNewCardPresent() && s_rfid.PICC_ReadCardSerial();
    if (!detected) {
        byte bufferATQA[2];
        byte bufferSize = sizeof(bufferATQA);
        MFRC522::StatusCode result = s_rfid.PICC_WakeupA(bufferATQA, &bufferSize);
        detected = (result == MFRC522::STATUS_OK || result == MFRC522::STATUS_COLLISION)
                   && s_rfid.PICC_ReadCardSerial();
    }
    if (!detected) {
        s_present = false;   // nothing on the reader right now — the NEXT
                              // detection (of anything) is a genuinely fresh
                              // tap, not a continuation of the last one
        return false;
    }

    char uid[24] = {0};
    for (byte i = 0; i < s_rfid.uid.size && i < 10; i++)
        snprintf(uid + i * 2, sizeof(uid) - i * 2, "%02X", s_rfid.uid.uidByte[i]);

    uint32_t now = millis();
    // A tap is "the same event" either because the card never left the
    // reader (s_present stayed true — WUPA re-wakes it every loop(), so a
    // purely time-based debounce would incorrectly expire after
    // TAP_DEBOUNCE_MS and fire a fresh tap for a card that's still sitting
    // right there — confirmed on the bench: repeated "tap ..." log lines
    // exactly TAP_DEBOUNCE_MS apart, one continuous physical presence), OR
    // because it was lifted and bounced back within the debounce window
    // (s_present went false momentarily, same UID, still recent).
    bool sameUid = strcmp(uid, s_lastUid) == 0;
    if (sameUid && (s_present || now - s_lastAt < TAP_DEBOUNCE_MS)) {
        if (!s_dupAnnounced) {
            s_dup = true;
            s_dupAnnounced = true;
        }
        s_present = true;
        release();       // a duplicate is still a card we selected — must still release it
        return false;
    }
    s_present = true;
    s_dupAnnounced = false;
    strncpy(s_lastUid, uid, sizeof s_lastUid - 1);
    s_lastAt = now;

    strncpy(out.uid, uid, sizeof out.uid - 1);
    out.recorded_at = clockw::now();      // DS3231, never millis()
    out.sms_sent    = false;
    snprintf(out.id, sizeof out.id, "%s-%lu-%lu",
             DEVICE_SHORT, (unsigned long)out.recorded_at, (unsigned long)++s_seq);
    // NOT released here — left selected so card::read() can optionally
    // pull the offline-fallback sector from this same session. Caller
    // MUST call release().
    return true;
}

void release() {
    s_rfid.PICC_HaltA();
    s_rfid.PCD_StopCrypto1();
}

MFRC522& instance() { return s_rfid; }

}
