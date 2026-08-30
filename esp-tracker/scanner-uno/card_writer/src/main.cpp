#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>

// Enrollment-station firmware: writes card.h's 48-byte offline-fallback
// format onto MIFARE Classic 1K cards, driven by card_writer_gui.py over
// USB serial. This sketch does no attendance logic at all — it is purely
// a serial-to-RFID bridge, because a PC can't drive an RC522 directly.
//
// Wiring matches scanner-uno/include/pins.h's proven RC522 hookup (same
// spare Uno+RC522 used for ../selftest_rfid/): SS=10, RST=9, SPI on the
// fixed 11/12/13 pins, 3.3V power to the RC522 ONLY (5V destroys it).
static const uint8_t PIN_SS  = 10;
static const uint8_t PIN_RST = 9;

// Must match scanner-uno/src/card.h and include/config.h::CARD_KEY_A
// EXACTLY, or every card this writes will fail to authenticate on the
// scanner (and this sketch's own READ-back verification will fail too).
// Spells "ESPTRK" in ASCII.
static const uint8_t CARD_KEY_A[6] = {0x45, 0x53, 0x50, 0x54, 0x52, 0x4B};

// Every MIFARE Classic card ships with this key on both Key A and Key B —
// a blank card will NEVER authenticate against CARD_KEY_A until this
// program rewrites its sector trailer. handleWrite() tries CARD_KEY_A
// first (an already-personalized card), then falls back to this (a fresh
// one), and always rewrites the trailer to CARD_KEY_A afterwards so the
// card authenticates directly next time.
static const uint8_t KEY_FACTORY_DEFAULT[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Standard MIFARE "transport configuration" access bits (C1C2C3=000 for
// the 3 data blocks, 001 for the trailer) — read/write/etc. via Key A or
// Key B on data blocks; Key A itself never readable back out. This is the
// same access-bits value every fresh card already ships with; only the
// keys are being changed here, not the permission structure.
static const uint8_t TRAILER_ACCESS_BITS[4] = {0xFF, 0x07, 0x80, 0x69};

static const uint8_t CARD_MAGIC   = 0xAC;
static const uint8_t CARD_VERSION = 0x01;
static const uint8_t CARD_SECTOR  = 1;          // absolute blocks 4,5,6 (7 = trailer)
static const uint32_t CARD_WAIT_MS = 5000;      // how long to wait for a card per command

static MFRC522 rfid(PIN_SS, PIN_RST);

// CRC-8/SMBUS: poly 0x07, init 0x00. Must match card.cpp's copy exactly —
// see that file's comment for why this particular CRC was picked.
static uint8_t crc8(const uint8_t* data, uint8_t len) {
    uint8_t crc = 0x00;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1);
    }
    return crc;
}

// The selftest_rfid (confirmed working) uses PICC_IsNewCardPresent()
// (REQA), while our original waitForCard() used PICC_WakeupA() (WUPA).
// Cheap RC522 clones sometimes have issues with one or the other, so try
// both: REQA first (fast, works for IDLE cards), then WUPA (covers HALTed
// cards left over from a previous command on the same reader session).
static bool waitForCard() {
    uint32_t start = millis();
    byte bufferATQA[2];
    while (millis() - start < CARD_WAIT_MS) {
        // Try REQA first — same path as the working selftest_rfid
        if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial())
            return true;
        // Fall back to WUPA — wakes HALTed cards that REQA can't see
        byte bufferSize = sizeof(bufferATQA);
        MFRC522::StatusCode result = rfid.PICC_WakeupA(bufferATQA, &bufferSize);
        if ((result == MFRC522::STATUS_OK || result == MFRC522::STATUS_COLLISION) && rfid.PICC_ReadCardSerial())
            return true;
        delay(50);
    }
    return false;
}

static void releaseCard() {
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
}

static void uidToHex(char* out, size_t outLen) {
    out[0] = '\0';
    char byteStr[3];
    for (byte i = 0; i < rfid.uid.size && i < 10; i++) {
        snprintf(byteStr, sizeof byteStr, "%02X", rfid.uid.uidByte[i]);
        strncat(out, byteStr, outLen - strlen(out) - 1);
    }
}

// Reads one line (up to len-1 chars, NUL-terminated) from Serial, blocking.
// Simple by design — this sketch only ever talks to one trusted GUI, not
// an open network, so there's no need for a non-blocking parser here.
static void readLine(char* buf, size_t len) {
    size_t i = 0;
    while (i < len - 1) {
        while (!Serial.available()) { /* wait */ }
        char c = Serial.read();
        if (c == '\n') break;
        if (c == '\r') continue;
        buf[i++] = c;
    }
    buf[i] = '\0';
}

// Parses "WRITE,<phone>,<studentId>,<name>". Phone accepts 10 digits
// (e.g. "9109943152") or 11 digits with a leading 0 (e.g. "09109943152",
// stripped to "9109943152" — the format card.h's BCD encoding expects).
// Returns false (and leaves a reason in err) on any malformed field —
// never partially writes.
static bool parseWrite(char* args, char* phone10, uint16_t& studentId, char* name, const char** err) {
    char* p = strtok(args, ",");
    if (!p) { *err = "BAD_PHONE"; return false; }

    // Accept 10 digits directly, or 11 digits starting with '0' (strip
    // the leading 0 to produce the 10-digit PH mobile after country code
    // that card.h's packed-BCD phone field expects).
    size_t len = strlen(p);
    const char* phoneDigits = p;
    if (len == 11 && p[0] == '0') {
        phoneDigits = p + 1;
    } else if (len != 10) {
        *err = "BAD_PHONE"; return false;
    }
    for (uint8_t i = 0; i < 10; i++)
        if (!isdigit((unsigned char)phoneDigits[i])) { *err = "BAD_PHONE"; return false; }
    memcpy(phone10, phoneDigits, 10);
    phone10[10] = '\0';

    p = strtok(nullptr, ",");
    if (!p || !isdigit((unsigned char)p[0])) { *err = "BAD_ID"; return false; }
    long id = atol(p);
    if (id < 0 || id > 65535) { *err = "BAD_ID"; return false; }
    studentId = (uint16_t)id;

    p = strtok(nullptr, "");   // rest of the line — name may legitimately be short, don't split further
    if (!p || strlen(p) == 0 || strlen(p) > 20) { *err = "BAD_NAME"; return false; }
    strncpy(name, p, 20);
    name[20] = '\0';

    return true;
}

static bool authSector(MFRC522::MIFARE_Key& key, uint8_t block) {
    return rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, block, &key, &rfid.uid) == MFRC522::STATUS_OK;
}

// After any halt + StopCrypto1 (failed auth fallback, or post-trailer-write
// re-auth), a full re-select via WUPA clears stale crypto state that
// StopCrypto1 alone doesn't.  Card must still physically be on the reader.
static bool reselectCard() {
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    uint32_t start = millis();
    byte bufferATQA[2];
    while (millis() - start < 2000) {
        if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial())
            return true;
        byte bufferSize = sizeof(bufferATQA);
        MFRC522::StatusCode result = rfid.PICC_WakeupA(bufferATQA, &bufferSize);
        if ((result == MFRC522::STATUS_OK || result == MFRC522::STATUS_COLLISION) && rfid.PICC_ReadCardSerial())
            return true;
        delay(30);
    }
    return false;
}

// Tries CARD_KEY_A first (card already personalized by this program),
// then the factory default (a fresh/blank card). Returns true and leaves
// the winning key in `key` on success.
static bool authSectorAny(MFRC522::MIFARE_Key& key, uint8_t block) {
    memcpy(key.keyByte, CARD_KEY_A, 6);
    if (authSector(key, block)) return true;
    if (!reselectCard()) return false;
    memcpy(key.keyByte, KEY_FACTORY_DEFAULT, 6);
    return authSector(key, block);
}

// Diagnostic: an AUTH_FAIL against BOTH keys almost always means the
// card isn't MIFARE Classic at all (cheap "RFID card" multipacks are
// frequently NTAG213/215/Ultralight instead, which don't support
// PCD_Authenticate/sectors the same way). Print what the reader actually
// sees so that's visible instead of a bare AUTH_FAIL.
static void printCardType() {
    MFRC522::PICC_Type type = rfid.PICC_GetType(rfid.uid.sak);
    Serial.print(F("TYPE:"));
    Serial.print(MFRC522::PICC_GetTypeName(type));
    Serial.print(F(" SAK=0x"));
    Serial.println(rfid.uid.sak, HEX);
}

static void handleWrite(char* args) {
    char phone10[11];
    uint16_t studentId;
    char name[21];
    const char* err;

    if (!parseWrite(args, phone10, studentId, name, &err)) {
        Serial.print(F("ERR:")); Serial.println(err);
        return;
    }

    if (!waitForCard()) { Serial.println(F("ERR:NO_CARD")); return; }

    uint8_t raw[48] = {0};
    raw[0] = CARD_MAGIC;
    raw[1] = CARD_VERSION;
    for (uint8_t i = 0; i < 5; i++) {
        raw[2 + i] = ((phone10[i * 2] - '0') << 4) | (phone10[i * 2 + 1] - '0');
    }
    raw[7] = studentId & 0xFF;
    raw[8] = (studentId >> 8) & 0xFF;
    memset(raw + 9, 0, 20);
    memcpy(raw + 9, name, strlen(name));
    raw[29] = crc8(raw, 29);
    // raw[30..47] already zeroed by the initializer above (reserved)

    MFRC522::MIFARE_Key key;
    const uint8_t firstBlock = CARD_SECTOR * 4;

    if (!authSectorAny(key, firstBlock)) {
        printCardType();
        releaseCard();
        Serial.println(F("ERR:AUTH_FAIL"));
        return;
    }

    for (uint8_t i = 0; i < 3; i++) {
        uint8_t block = firstBlock + i;
        if (rfid.MIFARE_Write(block, raw + i * 16, 16) != MFRC522::STATUS_OK) {
            releaseCard();
            Serial.println(F("ERR:WRITE_FAIL"));
            return;
        }
    }

    // Personalize the trailer so this card authenticates directly against
    // CARD_KEY_A next time, regardless of which key got us in above.
    // This MUST be the last write in this auth session — changing the
    // sector trailer invalidates the crypto session on many MIFARE
    // Classic cards (the NXP spec recommends re-auth after trailer writes).
    uint8_t trailer[16];
    memcpy(trailer, CARD_KEY_A, 6);
    memcpy(trailer + 6, TRAILER_ACCESS_BITS, 4);
    memcpy(trailer + 10, CARD_KEY_A, 6);
    if (rfid.MIFARE_Write(firstBlock + 3, trailer, 16) != MFRC522::STATUS_OK) {
        releaseCard();
        Serial.println(F("ERR:WRITE_FAIL"));
        return;
    }

    // The trailer write changed the sector's key material — the current
    // crypto session (established with the OLD key) is no longer trusted
    // by many card implementations.  Release, re-select, and authenticate
    // with the NEW key (CARD_KEY_A) before reading back to verify.
    // This matches the Arduino MFRC522 example's pattern: always
    // re-authenticate before reads after writes.
    releaseCard();
    if (!reselectCard()) {
        Serial.println(F("ERR:RESELECT_FAIL"));
        return;
    }
    memcpy(key.keyByte, CARD_KEY_A, 6);
    if (!authSector(key, firstBlock)) {
        releaseCard();
        Serial.println(F("ERR:AUTH_FAIL"));
        return;
    }

    // Verify by reading back before declaring success — a write that looks
    // OK to the RC522 but doesn't stick is worse than a write that visibly
    // fails, since it silently ships a card the scanner can't trust.
    uint8_t verify[48];
    for (uint8_t i = 0; i < 3; i++) {
        uint8_t block = firstBlock + i;
        uint8_t size = 18, buf[18];
        if (rfid.MIFARE_Read(block, buf, &size) != MFRC522::STATUS_OK) {
            releaseCard();
            Serial.println(F("ERR:VERIFY_FAIL"));
            return;
        }
        memcpy(verify + i * 16, buf, 16);
    }
    if (memcmp(raw, verify, 48) != 0) {
        releaseCard();
        Serial.println(F("ERR:VERIFY_MISMATCH"));
        return;
    }

    char uidHex[24];
    uidToHex(uidHex, sizeof uidHex);
    releaseCard();
    Serial.print(F("OK:")); Serial.println(uidHex);
}

static void handleRead() {
    if (!waitForCard()) { Serial.println(F("ERR:NO_CARD")); return; }

    MFRC522::MIFARE_Key key;
    const uint8_t firstBlock = CARD_SECTOR * 4;

    if (!authSectorAny(key, firstBlock)) {
        printCardType();
        releaseCard();
        Serial.println(F("ERR:AUTH_FAIL"));
        return;
    }

    uint8_t raw[48];
    for (uint8_t i = 0; i < 3; i++) {
        uint8_t block = firstBlock + i;
        uint8_t size = 18, buf[18];
        if (rfid.MIFARE_Read(block, buf, &size) != MFRC522::STATUS_OK) {
            releaseCard();
            Serial.println(F("ERR:READ_FAIL"));
            return;
        }
        memcpy(raw + i * 16, buf, 16);
    }
    releaseCard();

    if (raw[0] != CARD_MAGIC || raw[1] != CARD_VERSION) { Serial.println(F("ERR:BAD_MAGIC")); return; }
    if (crc8(raw, 29) != raw[29]) { Serial.println(F("ERR:BAD_CRC")); return; }

    char phone[14] = "+63";
    for (uint8_t i = 0; i < 5; i++) {
        uint8_t b = raw[2 + i];
        phone[3 + i * 2]     = '0' + (b >> 4);
        phone[3 + i * 2 + 1] = '0' + (b & 0x0F);
    }
    phone[13] = '\0';
    uint16_t studentId = (uint16_t)raw[7] | ((uint16_t)raw[8] << 8);
    char name[21];
    memcpy(name, raw + 9, 20);
    name[20] = '\0';

    Serial.print(F("DATA:")); Serial.print(phone);
    Serial.print(','); Serial.print(studentId);
    Serial.print(','); Serial.println(name);
}

void setup() {
    Serial.begin(9600);
    delay(200);           // let the USB-serial side settle after reset
    SPI.begin();
    rfid.PCD_Init();
    delay(50);

    // Verify SPI communication — a dead register read means wiring wrong
    // or 5V fed to RC522 (which destroys it silently).
    byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    if (v == 0x00 || v == 0xFF) {
        Serial.print(F("ERR:RC522_DEAD ver=0x"));
        Serial.println(v, HEX);
        return;    // don't announce READY — GUI will see no response
    }
    rfid.PCD_AntennaOn();
    Serial.println(F("READY"));
}

void loop() {
    char line[64];
    readLine(line, sizeof line);
    if (line[0] == '\0') return;

    if (strncmp(line, "WRITE,", 6) == 0) {
        handleWrite(line + 6);
    } else if (strcmp(line, "READ") == 0) {
        handleRead();
    } else if (strcmp(line, "PING") == 0) {
        Serial.println(F("PONG"));
    } else {
        Serial.println(F("ERR:BAD_CMD"));
    }
}
