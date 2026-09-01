#include "lcd.h"
#include "../include/config.h"
#include <LiquidCrystal_I2C.h>
#include <string.h>

static LiquidCrystal_I2C s_lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);
static bool s_ok = false;

// Pads/truncates to exactly LCD_COLS chars so a shorter new message fully
// overwrites a longer old one — the LCD doesn't clear on its own between
// writes to the same row.
static void writeRow(uint8_t row, const char* text) {
    char buf[21];
    uint8_t n = 0;
    for (; n < LCD_COLS && text && text[n]; n++) buf[n] = text[n];
    for (; n < LCD_COLS; n++) buf[n] = ' ';
    buf[n] = 0;
    s_lcd.setCursor(0, row);
    s_lcd.print(buf);
}

namespace lcd {

void begin() {
    s_lcd.init();
    s_lcd.backlight();
    s_lcd.clear();
    writeRow(0, "Scanner ready");
    s_ok = true;
}

void show(const char* line1, const char* line2) {
    if (!s_ok) return;
    writeRow(0, line1);
    if (line2) writeRow(1, line2);
}

void clear() {
    if (!s_ok) return;
    s_lcd.clear();
}

}
