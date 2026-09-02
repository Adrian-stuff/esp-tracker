#pragma once

namespace ble_debug {
void begin();
void service();
void dbg(const char* fmt, ...);
}
