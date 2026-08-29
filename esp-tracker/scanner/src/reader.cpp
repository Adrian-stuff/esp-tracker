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
static uint32_t s_seq         = 0;

namespace reader {

void begin() {
    SPI.begin(PIN_RFID_SCK, PIN_RFID_MISO, PIN_RFID_MOSI, PIN_RFID_SS);
    s_rfid.PCD_Init();
}

bool sawDuplicate() { return s_dup; }

bool poll(Tap& out) {
    s_dup = false;
    if (!s_rfid.PICC_IsNewCardPresent() || !s_rfid.PICC_ReadCardSerial()) return false;

    char uid[24] = {0};
    for (byte i = 0; i < s_rfid.uid.size && i < 10; i++)
        snprintf(uid + i * 2, sizeof(uid) - i * 2, "%02X", s_rfid.uid.uidByte[i]);
    s_rfid.PICC_HaltA();
    s_rfid.PCD_StopCrypto1();

    // A card resting on the reader must not generate hundreds of events — but a
    // legitimate re-tap minutes later must still work.
    uint32_t now = millis();
    if (strcmp(uid, s_lastUid) == 0 && now - s_lastAt < TAP_DEBOUNCE_MS) {
        s_dup = true;
        return false;
    }
    strncpy(s_lastUid, uid, sizeof s_lastUid - 1);
    s_lastAt = now;

    strncpy(out.uid, uid, sizeof out.uid - 1);
    out.recorded_at = clockw::now();      // DS3231, never millis()
    out.sms_sent    = false;              // notify::onTap decides, after this
    snprintf(out.id, sizeof out.id, "%s-%lu-%lu",
             DEVICE_SHORT, (unsigned long)out.recorded_at, (unsigned long)++s_seq);
    return true;
}

}
