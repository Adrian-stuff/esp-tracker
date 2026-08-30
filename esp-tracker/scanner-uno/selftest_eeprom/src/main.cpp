// Internal EEPROM isolation test. Probes a few addresses near the very end
// of the 1024-byte EEPROM — safely past eeprom_layout.h's QUEUE_END (~950),
// so this can never collide with the real firmware's roster cache or
// offline tap queue, even run repeatedly on a device that already has data.

#include <Arduino.h>
#include <EEPROM.h>

static const int PROBE_ADDRS[] = {1000, 1010, 1020, 1023};

void runTest() {
    Serial.println(F("\n=== EEPROM isolation test ==="));
    bool allOk = true;
    for (int addr : PROBE_ADDRS) {
        uint8_t original = EEPROM.read(addr);
        uint8_t pattern  = 0xA5;
        EEPROM.write(addr, pattern);
        uint8_t back = EEPROM.read(addr);
        EEPROM.write(addr, original);   // restore

        Serial.print(F("  addr "));
        Serial.print(addr);
        Serial.print(F(": "));
        if (back == pattern) {
            Serial.println(F("PASS"));
        } else {
            Serial.print(F("FAIL - wrote 0xA5, read back 0x"));
            Serial.println(back, HEX);
            allOk = false;
        }
    }
    Serial.println(allOk ? F("EEPROM: all probes PASS") : F("EEPROM: at least one probe FAILED - chip likely dead"));
    Serial.println(F("send any character to rerun"));
}

void setup() {
    Serial.begin(115200);
    delay(600);
    runTest();
}

void loop() {
    if (Serial.available()) {
        while (Serial.available()) Serial.read();
        runTest();
    }
    delay(30);
}
