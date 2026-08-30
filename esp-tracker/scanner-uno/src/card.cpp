#include "card.h"
#include "reader.h"
#include "../include/config.h"
#include <MFRC522.h>

// CRC-8/SMBUS: poly 0x07, init 0x00, no reflect, no xorout. Nothing fancy —
// picked only because it's the simplest CRC8 to reproduce byte-for-byte in
// the Python writer tool without pulling in a library on either side. If
// this ever changes, both card_writer.py and this function must change
// together or every card in the field silently stops validating.
static uint8_t crc8(const uint8_t* data, uint8_t len) {
    uint8_t crc = 0x00;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1);
    }
    return crc;
}

namespace card {

bool read(CardData& out) {
    MFRC522& rfid = reader::instance();

    MFRC522::MIFARE_Key key;
    for (uint8_t i = 0; i < 6; i++) key.keyByte[i] = CARD_KEY_A[i];

    // Sector CARD_SECTOR's three data blocks are absolute blocks 4,5,6
    // (block 7 is that sector's trailer, holds keys not data). One AUTH
    // covers the whole sector — only issue it once, on the first block.
    const uint8_t firstBlock = CARD_SECTOR * 4;
    uint8_t raw[48];

    for (uint8_t i = 0; i < 3; i++) {
        uint8_t block = firstBlock + i;
        if (i == 0) {
            auto status = rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, block, &key, &rfid.uid);
            if (status != MFRC522::STATUS_OK) return false;
        }
        uint8_t bufSize = 18;   // MIFARE_Read wants room for 16 data + 2 CRC_A bytes
        uint8_t buf[18];
        auto status = rfid.MIFARE_Read(block, buf, &bufSize);
        if (status != MFRC522::STATUS_OK) return false;
        memcpy(raw + i * 16, buf, 16);
    }

    if (raw[0] != CARD_MAGIC || raw[1] != CARD_VERSION) return false;
    if (crc8(raw, 29) != raw[29]) return false;

    char phone[14];
    strcpy(phone, "+63");
    for (uint8_t i = 0; i < 5; i++) {
        uint8_t b = raw[2 + i];
        phone[3 + i * 2]     = '0' + (b >> 4);
        phone[3 + i * 2 + 1] = '0' + (b & 0x0F);
    }
    phone[13] = '\0';
    strncpy(out.phone, phone, sizeof out.phone - 1);
    out.phone[sizeof out.phone - 1] = '\0';

    out.studentId = (uint16_t)raw[7] | ((uint16_t)raw[8] << 8);

    memcpy(out.name, raw + 9, 20);
    out.name[20] = '\0';

    return true;
}

}
