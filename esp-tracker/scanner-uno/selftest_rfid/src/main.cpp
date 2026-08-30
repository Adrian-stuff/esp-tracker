// RC522-only isolation test. Same pins as scanner-uno/include/pins.h:
// SCK=13 MISO=12 MOSI=11 SS=10 RST=9 (hardware SPI, fixed on the Uno).
//
// Prints the reader's version register once at boot, then continuously
// prints any card UID tapped — same as the standard MFRC522 "DumpInfo"
// example, just trimmed to what this project actually needs.

#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <avr/wdt.h>

#define PIN_RFID_SS  10
#define PIN_RFID_RST 9

MFRC522 rfid(PIN_RFID_SS, PIN_RFID_RST);

void setup() {
    wdt_disable();
    Serial.begin(115200);
    delay(600);
    Serial.println(F("\n=== RFID-ONLY isolation test ==="));

    SPI.begin();
    rfid.PCD_Init();
    delay(50);

    byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    if (v == 0x00 || v == 0xFF) {
        Serial.print(F("FAIL - version 0x"));
        Serial.print(v, HEX);
        Serial.println(F(" - no response. Check wiring / 3V3 ONLY, 5V destroys this module"));
        return;
    }
    Serial.print(F("PASS - version 0x"));
    Serial.print(v, HEX);
    Serial.println(v == 0x91 ? F(" (v1.0)") : v == 0x92 ? F(" (v2.0)") : F(" (clone, usually fine)"));
    rfid.PCD_AntennaOn();
    Serial.println(F("antenna on - tap a card any time\n"));
}

void loop() {
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
        char uid[24] = {0};
        for (byte i = 0; i < rfid.uid.size && i < 10; i++)
            snprintf(uid + i * 2, sizeof(uid) - i * 2, "%02X", rfid.uid.uidByte[i]);
        Serial.print(F("CARD  uid=")); Serial.print(uid);
        Serial.print(F("  type="));
        Serial.println(rfid.PICC_GetTypeName(rfid.PICC_GetType(rfid.uid.sak)));
        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();
    }
    delay(30);
}
