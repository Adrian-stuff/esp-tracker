#pragma once

// BLE Serial — Nordic UART Service (NUS) over BLE.
//
// External devices (phone apps, laptops) connect via BLE and send commands
// through a virtual serial port. The scanner receives the data and routes
// SMS commands through the existing smsq queue.
//
// Protocol (newline-terminated):
//   SMS:<number> <message>   — enqueue an SMS via the SIM900
//   STATUS                   — return scanner status
//   PING                     — respond with PONG
//
// Responses and debug output are sent back to the connected BLE client.

namespace ble_serial {
    void begin();
    void service();    // call every loop()
    void notify(const char* msg);   // send text to connected BLE client
}
