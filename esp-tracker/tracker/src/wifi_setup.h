#pragma once
#include <stdbool.h>

namespace wifi_setup {

// Check if SOS button is held at boot. Call at the very start of setup(),
// before any WiFi or NimBLE init. Returns true if button is LOW (held).
bool buttonHeldAtBoot();

// Enter captive portal config mode. Blocks for up to SETUP_TIMEOUT_MS,
// then returns so the device can reboot into normal mode.
//
// WiFi mode is set to AP+STA: the AP serves the config page while STA
// continues scanning for motion detection. Both coexist on ESP32.
void enter();

}
