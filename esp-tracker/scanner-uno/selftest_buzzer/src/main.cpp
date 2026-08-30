// Buzzer-only isolation test. Pin 6, direct GPIO drive via tone()/noTone()
// — no resistor or transistor needed for a small piezo buzzer (a few mA,
// well under the Uno's per-pin comfort limit).
//
// Press any key + Enter in the Serial Monitor to chirp again.

#include <Arduino.h>

#define PIN_BUZZER 6

void chirp() {
    Serial.println(F("chirp"));
    tone(PIN_BUZZER, 1800); delay(120);
    tone(PIN_BUZZER, 2400); delay(160);
    noTone(PIN_BUZZER);
}

void setup() {
    Serial.begin(115200);
    delay(600);
    pinMode(PIN_BUZZER, OUTPUT);
    Serial.println(F("\n=== BUZZER-ONLY isolation test ==="));
    chirp();
    Serial.println(F("send any character to chirp again"));
}

void loop() {
    if (Serial.available()) {
        while (Serial.available()) Serial.read();   // drain the whole line
        chirp();
    }
    delay(30);
}
