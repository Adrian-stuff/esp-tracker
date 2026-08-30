#include <Arduino.h>
#include "reader.h"
#include "card.h"

// Drives the RC522 directly through reader::instance() rather than
// reader::poll() — poll() pulls in clockw::now() (DS1302), which this
// bench setup doesn't have wired. card.cpp's card::read() only needs the
// MFRC522 to already have a card selected, which PICC_IsNewCardPresent()
// + PICC_ReadCardSerial() does on their own.

void setup() {
    Serial.begin(9600);
    reader::begin();
    delay(200);
    Serial.println(F("READY - tap the card written by card_writer"));
}

void loop() {
    MFRC522& rfid = reader::instance();
    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

    CardData cd;
    bool ok = card::read(cd);
    reader::release();

    if (ok) {
        Serial.print(F("OK phone=")); Serial.print(cd.phone);
        Serial.print(F(" id="));      Serial.print(cd.studentId);
        Serial.print(F(" name="));    Serial.println(cd.name);
    } else {
        Serial.println(F("card::read() FAILED"));
    }
    delay(1000);
}
