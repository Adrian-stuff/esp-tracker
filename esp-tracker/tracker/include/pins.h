#pragma once
// Pin map — ESP32 DevKit v1 (WROOM-32).
//
// Two ESP32 traps are encoded here, do not "tidy" them away:
//   * ADC2 pins cannot be read while Wi-Fi is active. Battery sense must be on GPIO 32-39.
//   * Deep-sleep wake requires an RTC-capable GPIO. Hence the SOS button on 33.

// ---- SIM800L (UART2) -------------------------------------------------------
// POWER: wire VCC DIRECTLY to the LiPo, never to 3V3. 1000-2200uF bulk cap
// close to the module. See README "Power warnings".
#define PIN_MODEM_RX      16   // SIM800L TX -> ESP32 RX
#define PIN_MODEM_TX      17   // ESP32 TX  -> SIM800L RX  *** VIA LEVEL SHIFTER ***
#define PIN_MODEM_PWRKEY  23   // pulse LOW ~1s to toggle module power
#define PIN_MODEM_DTR     19   // held LOW to wake from AT+CSCLK=1 sleep
#define PIN_MODEM_EN      32   // high-side MOSFET gate, full power cut

// ---- NEO-6M GNSS (UART1, remapped off the flash pins) ----------------------
#define PIN_GPS_RX        25   // NEO-6M TX -> ESP32 RX
#define PIN_GPS_TX        26   // ESP32 TX  -> NEO-6M RX
#define PIN_GPS_EN        27   // MOSFET gate. V_BCKP stays powered regardless,
                               // otherwise every fix becomes a 27s cold start.

// ---- Human interface -------------------------------------------------------
#define PIN_SOS_BUTTON    33   // RTC-capable -> ext0 deep-sleep wake. Active LOW, pull-up.

// Child-facing feedback — RGB LED (common anode, active LOW).
// No piezo buzzer (GPIO 12 is a boot strapping pin — do NOT use).
//   Common anode → 3.3V
//   R : red leg — GPIO LOW to light
//   G : green leg — GPIO LOW to light
//   B : blue leg — GPIO LOW to light
#define PIN_LED_R         13   // red leg
#define PIN_LED_G         14   // green leg
#define PIN_LED_B         15   // blue leg
#define PIN_LED            2   // onboard, debug only — not the child-facing one

// ---- Power sense -----------------------------------------------------------
// NOT POPULATED on current hardware — there is no physical voltage divider
// on this board, no battery sensor is fitted. This pin assignment and ratio
// are RESERVED for a future revision that adds one; nothing currently reads
// this pin. Battery is estimated in software instead — see battery.cpp,
// and BATTERY_HAS_ADC_SENSOR in config.h, which must stay false until a
// board with the real divider exists.
#define PIN_VBAT_SENSE    34   // ADC1, input-only, if/when populated
#define VBAT_DIVIDER_RATIO 2.0f  // matches a 100k/100k divider, if/when populated
