#include "smsq.h"
#include "../include/pins.h"
#include "../include/config.h"
#include <Arduino.h>
#include <HardwareSerial.h>

static HardwareSerial Sim(2);

struct Msg { char to[20]; char body[152]; char ref[16]; uint8_t attempts; };
struct Res { char ref[16]; bool sent; };
static Res     s_res[SMS_QUEUE_DEPTH];
static uint8_t s_rHead = 0, s_rCount = 0;

static void report(const Msg& m, bool sent) {
    if (!m.ref[0] || s_rCount >= SMS_QUEUE_DEPTH) return;
    Res& r = s_res[(s_rHead + s_rCount) % SMS_QUEUE_DEPTH];
    strncpy(r.ref, m.ref, sizeof r.ref - 1); r.ref[sizeof r.ref - 1] = 0;
    r.sent = sent;
    s_rCount++;
}
static Msg     s_q[SMS_QUEUE_DEPTH];
static uint8_t s_head = 0, s_count = 0;

enum class St : uint8_t { Boot, Idle, SendCmd, WaitPrompt, SendBody, WaitOk, Cooldown };
static St       s_st      = St::Boot;
static uint32_t s_at      = 0;      // when the current step started
static bool     s_ready   = false;
static int8_t   s_csq     = -1;
static uint32_t s_lastReg = 0;
static String   s_rx;

static void write(const char* cmd) {
    while (Sim.available()) Sim.read();
    Sim.print(cmd);
    Sim.print("\r\n");
}

static bool sawAny(const char* a, const char* b) {
    return s_rx.indexOf(a) >= 0 || (b && s_rx.indexOf(b) >= 0);
}

namespace smsq {

bool begin() {
    // Not fitted yet: stay out of the way entirely rather than pulsing PWRKEY
    // into thin air and polling a UART nobody is on.
    if (!SIM900_PRESENT) { s_st = St::Idle; s_ready = false; return false; }

    // GPIO32 (PWRKEY) floats during ESP32 boot, randomly toggling the SIM900.
    // Hold it HIGH immediately to prevent that. We only pulse LOW later if the
    // modem fails to respond to AT.
    pinMode(PIN_SIM_PWRKEY, OUTPUT);
    digitalWrite(PIN_SIM_PWRKEY, HIGH);

    Sim.begin(9600, SERIAL_8N1, PIN_SIM_RX, PIN_SIM_TX);

    // Cancel any pending AT+CMGS state — a previous failed send can leave the
    // modem waiting for a body or Ctrl-Z, and every subsequent AT returns "> ".
    Sim.write(0x1B); delay(200);
    Sim.write(0x1B); delay(200);
    while (Sim.available()) Sim.read();

    s_st = St::Boot;
    s_at = millis();
    return true;
}

bool   ready()  { return s_ready; }
size_t depth()  { return s_count; }
int8_t signalQuality() { return s_csq; }
HardwareSerial& serial() { return Sim; }
bool isIdle() { return s_st == St::Idle; }

bool enqueue(const char* number, const char* text, const char* ref) {
    if (!SIM900_PRESENT) return false;
    if (s_count >= SMS_QUEUE_DEPTH) return false;
    Msg& m = s_q[(s_head + s_count) % SMS_QUEUE_DEPTH];
    strncpy(m.to, number, sizeof m.to - 1);   m.to[sizeof m.to - 1] = 0;
    strncpy(m.body, text, sizeof m.body - 1); m.body[sizeof m.body - 1] = 0;
    if (ref) { strncpy(m.ref, ref, sizeof m.ref - 1); m.ref[sizeof m.ref - 1] = 0; }
    else m.ref[0] = 0;
    m.attempts = 0;
    s_count++;
    return true;
}

bool takeResult(char* ref, size_t refLen, bool* sent) {
    if (!s_rCount) return false;
    Res& r = s_res[s_rHead];
    strncpy(ref, r.ref, refLen - 1); ref[refLen - 1] = 0;
    *sent = r.sent;
    s_rHead = (s_rHead + 1) % SMS_QUEUE_DEPTH;
    s_rCount--;
    return true;
}

static void drop() { s_head = (s_head + 1) % SMS_QUEUE_DEPTH; s_count--; }

void service() {
    if (!SIM900_PRESENT) return;
    while (Sim.available()) { s_rx += (char)Sim.read(); if (s_rx.length() > 400) s_rx.remove(0, 200); }
    uint32_t now = millis();

    switch (s_st) {
    case St::Boot:
        // First pass: let the module stabilize, then probe with AT.
        // If the modem was already on (common after ESP32 reset — PWRKEY
        // held HIGH to prevent floating), it responds immediately and we
        // skip the power cycle entirely. The ESC bytes in begin() already
        // cleared any stuck AT+CMGS state.
        if (now - s_at < 3000) return;          // let the module stabilize
        if (now - s_at < 6000) {
            s_rx = ""; write("AT");
            return;                             // wait for response next call
        }
        if (sawAny("OK", "+CREG")) {
            // Modem already on — configure and go
            s_rx = "";
            write("ATE0");
            write("AT+CMGF=1");
            s_rx = ""; write("AT+CREG?");
            s_st = St::Idle; s_at = now; s_lastReg = now;
            return;
        }
        // No response after 6 s — power cycle via PWRKEY toggle
        s_rx = "";
        digitalWrite(PIN_SIM_PWRKEY, LOW);  delay(1200);
        digitalWrite(PIN_SIM_PWRKEY, HIGH);
        s_st = St::Boot; s_at = now;             // re-enter, re-probe after 6 s
        return;

    case St::Idle: {
        // Registration + signal, polled gently. Mains powered, so unlike the
        // tracker there is no reason to sleep the modem between messages.
        if (now - s_lastReg > 20000) {
            s_lastReg = now;
            s_ready = sawAny("+CREG: 0,1", "+CREG: 0,5");
            int i = s_rx.indexOf("+CSQ:");
            if (i >= 0) s_csq = s_rx.substring(i + 5, s_rx.indexOf(',', i)).toInt();
            s_rx = ""; write("AT+CREG?"); delay(40); write("AT+CSQ");
        }
        if (!s_count || !s_ready) return;
        s_rx = "";
        {
            char cmd[40];
            snprintf(cmd, sizeof cmd, "AT+CMGS=\"%s\"", s_q[s_head].to);
            write(cmd);
        }
        s_st = St::WaitPrompt; s_at = now;
        return;
    }

    case St::WaitPrompt:
        if (s_rx.indexOf('>') >= 0) {
            Sim.print(s_q[s_head].body);
            Sim.write(26);                      // Ctrl-Z ends the message
            s_rx = "";
            s_st = St::WaitOk; s_at = now;
        } else if (now - s_at > 8000) {
            Sim.write(27);                      // ESC, abandon the prompt
            s_st = St::Cooldown; s_at = now;
        }
        return;

    case St::WaitOk:
        if (sawAny("+CMGS:", "OK")) {           // sent
            report(s_q[s_head], true);
            drop();
            s_st = St::Cooldown; s_at = now;
        } else if (s_rx.indexOf("ERROR") >= 0 || now - s_at > SMS_SEND_TIMEOUT_MS) {
            if (++s_q[s_head].attempts >= SMS_MAX_ATTEMPTS) {
                report(s_q[s_head], false);      // let the server pay instead
                drop();
            }
            s_st = St::Cooldown; s_at = now;
        }
        return;

    case St::Cooldown:
        if (now - s_at > 1500) { s_rx = ""; s_st = St::Idle; }
        return;

    default:
        s_st = St::Idle;
        return;
    }
}

}
