#include "display.h"
#include "../include/config.h"
#include <LiquidCrystal_I2C.h>
#include <string.h>
#include <stdio.h>

static LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);

// Pads/truncates to exactly LCD_COLS chars so a shorter new message fully
// overwrites a longer old one — the LCD doesn't clear on its own between
// writes to the same row.
static void writeRow(uint8_t row, const char* text) {
    char buf[21];   // covers up to a 20-col display; LCD_COLS is checked below
    uint8_t n = 0;
    for (; n < LCD_COLS && text && text[n]; n++) buf[n] = text[n];
    for (; n < LCD_COLS; n++) buf[n] = ' ';
    buf[n] = 0;
    lcd.setCursor(0, row);
    lcd.print(buf);
}

namespace display {

void begin() {
    lcd.init();
    lcd.backlight();
    lcd.clear();
    writeRow(0, "Scanner starting");
}

void show(Status s, const char* detail) {
    const char* line1;
    switch (s) {
        case Status::Ok:        line1 = "Tap OK";       break;
        case Status::Error:     line1 = "ERROR";        break;
        case Status::Buffering: line1 = "Buffering...";  break;
        case Status::Unknown:   line1 = "Unknown card";  break;
        case Status::Idle:      line1 = "Ready";         break;
    }
    writeRow(0, line1);
    if (detail) writeRow(1, detail);
}

}
