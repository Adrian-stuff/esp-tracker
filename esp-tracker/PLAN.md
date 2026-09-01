# Child Safety Tracker — System Plan

**Status:** v4
**Date:** 2026-08-28
**Supersedes:** v3 (SIM7080G, MQTT), v2 (dual-path), v1 (FMDN-only)

**Change summary:** v4 reflects the real hardware and the real country.

- **SIM800L instead of SIM7080G** (no stock). 2G-only — which is fine, because we are in the
  **Philippines**, where 2G is widely deployed and LTE-M/NB-IoT effectively are not.
- **HTTP instead of MQTT.** Once GPRS became bursty rather than persistent, the broker earned
  nothing: no broker to run, no separate ingest worker, and the HTTP 200 *is* the ack.
- **SMS promoted to a first-class channel in both directions** — SOS out, "locate now" in. This
  is the one place the downgraded modem makes the design better.
- **An RFID attendance scanner** (ESP32 + RC522) added as a second device.
- **Places are named by the parent from the dashboard**, out of the networks the tracker actually
  reports seeing — closing the loop on the indoor positioning the whole design leans on (§3.3).

---

## 0. How this design arrived here

**v1** used FMDN as the sole location and alerting channel.
**v2** kept FMDN as a fallback beneath a cellular primary.
**v3** cuts FMDN. The reasoning is worth recording, because "add a free backup radio" is a
tempting instinct that doesn't survive contact with the specifics.

### Why FMDN can't carry the SOS (established in v1 → v2)
FMDN is a **one-way network**. The tag advertises; it never transmits arbitrary data and never
receives an acknowledgement. There is no field in the advertisement payload that can be made to
mean "SOS" and reach you. Reports surface only when a stranger's phone scans the tag and uploads.
This is a property of the protocol, not a tunable parameter.

### Why FMDN isn't worth keeping as a fallback either (new in v3)

1. **Its coverage is a subset of what it was supposed to back up.** FMDN detection needs a nearby
   Android device opted into the network — and on many devices the network setting defaults to
   *high-traffic areas only*, so it needs several. Places dense with phone-carrying people are
   overwhelmingly places with cell coverage. The dead zones a fallback exists for — rural roads,
   empty streets at night — are precisely where no passer-by will ever scan the tag. It is
   weakest exactly where it is needed.

2. **Its one real niche is better served by something already in the design.** Dense indoors with
   poor cellular — a mall interior, a basement — is genuine FMDN territory. But that is what the
   Wi-Fi AP scan handles: better accuracy, no third party, no extra radio, and it reports through
   a channel we control. The residual niche is close to empty.

3. **It introduces a problem that otherwise does not exist.** A GNSS + cellular tracker advertises
   nothing and therefore trips no unwanted-tracker detection. Adding an FMDN beacon opts the child
   into "unknown tracker travelling with you" alerts on every nearby phone, plus a finder's
   ability to make the device play a sound and reveal itself. That is the most socially awkward
   failure mode in the whole project, traded for its least reliable data.

Add the carrying cost — unofficial provisioning, an expiring Google session, a decryption path,
extra source handling in the schema, extra stale-data states in the UI, and a dependency that can
break without notice — and it is roughly a third of the project's complexity attached to its worst
data.

### What we give up, honestly
FMDN answers *"where did the device go?"* — dropped on a bus, left in a mall. That is a
lost-property question, not a child-safety one, and dropping it means **a tracker that is off,
dead, or out of coverage is simply gone.** Mitigations, none of which are FMDN:
- Last-known cellular position with an explicit age, always visible.
- On-board buzzer, triggerable remotely while it still has network.
- A short-range BLE "find mode" the parent can trigger from the app when they're nearby
  (advertising only, only while actively searching, never persistently — see §7).

> **Worth saying once:** a commercial LTE kids' watch does this today with carrier-grade
> reliability and a support line at 2am. If the goal is a working safety device *this month*,
> buy one. If the goal is to build and own the stack — a legitimate goal — this plan is how.
> Either way, don't let the DIY unit be the only safety measure while it is unproven.
>
> And if the appeal of this project was the FMDN reverse-engineering itself, that is a fine
> reason to build it. Build it as its own toy on a spare ESP32; don't make a child's safety
> depend on it.

---

## 1. Architecture

```
   ┌──────────────────────────────────────┐
   │        ESP32 tracker (worn)          │
   │  SOS button (2 s hold)               │        SMS — direct to parent
   │                                      │───────────────────────────────────┐
   │  Position sources, priority order:   │  no GPRS · no TLS · no server     │
   │    1  GNSS (NEO-6M)   outdoors       │  ~5-10 s, almost unkillable       │
   │    2  Wi-Fi AP scan   indoors        │                                   │
   │    3  BLE anchors     known places   │                                   │
   │    4  Cell ID         floor          │                                   │
   │                                      │                                   │
   │  SIM800L (2G) + flash retry queue    │                                   │
   └───────────────┬──────────────────────┘                                   │
                   │  HTTPS burst, TLS on the ESP32                           │
                   │  200 IS the ack                                          │
                   ▼                                                          │
   ┌──────────────────────┐   ┌─────────────────────┐                         │
   │  FastAPI             │──▶│  Alert dispatcher   │──── push · SMS · voice ─┤
   │  ingest + parent API │   │  escalation ladder  │                         │
   │  (no broker)         │   │  halted by ack      │                         ▼
   └───────┬──────────────┘   └─────────────────────┘              ┌────────────────────┐
           │ write                                                 │  Parent PWA        │
           ▼                                                       │  live map · ack    │
   ┌──────────────────────┐                                        └────────────────────┘
   │  SQLite / PostgreSQL │            "Locate now" ─── SMS ──▶ wakes the modem from
   │  source-tagged       │                                     CSCLK sleep via RI
   └──────────────────────┘
           ▲
           │ HTTPS burst
   ┌───────┴──────────────┐
   │  ESP32 + RC522       │   fixed attendance station at the school gate
   │  scanner (mains)     │   DS3231 RTC · offline tap buffer
   └──────────────────────┘

   ┌───────────────────────┐
   │ Geolocation provider  │◀── BSSID lists, ONLY on a known-place cache miss
   └───────────────────────┘
```

One data uplink plus SMS as an out-of-band channel. Every hop is a system we control, except
the geolocation provider — consulted only when the on-device place cache misses (§3.3).

**Why HTTP and not MQTT.** Carrier NAT drops an idle GPRS TCP session after 1–5 minutes, so a
persistent broker session needs ~60 s keepalives — and those keepalives stop the modem ever
reaching `AT+CSCLK` sleep, the biggest power lever available. Once the connection has to be
bursty anyway, MQTT's advantages evaporate and HTTP is strictly simpler: no broker to run or
secure, the ingest endpoint is just another FastAPI route, and **the HTTP 200 is the
server-level ack** — no "QoS 1 confirms delivery to the broker, not the server" trap to fall
into. Batch aggressively so the 3–8 s TLS handshake is amortised over many points, and give
every event an id so a retried POST is idempotent.

---

## 1b. Scanner alternative — Arduino Uno + SIM800L (cellular-only)

Not hypothetical: both ESP32 boards on hand failed independent hardware diagnosis (bad EN/regulator
circuit on one, a dead or disconnected chip behind an otherwise-healthy USB-UART bridge on the
other — neither is a wiring, driver, cable, or host OS problem). The Arduino Uno is the one MCU
confirmed working end to end. This section documents the fallback so the scanner can ship on it if a
replacement ESP32 doesn't arrive in time, without treating it as a downgrade in the data contract.

Two components originally planned for this build turned out to be the wrong part, both discovered
by testing against real hardware rather than assumption:

- **The "I2C LED" is a PCF8574 LCD backpack**, not an RGB driver — its 8 GPIO lines are hard-wired
  to LCD control signals, not free outputs. Status feedback is short text on a 16x2 screen instead
  of a colour. See `scanner-uno/src/display.h`.
- **The RTC is a DS1302**, not a DS3231 — 3-wire (CE/IO/SCLK), not I2C at all, which is why it
  never showed up in any I2C bus scan. Confirmed working on real hardware: read, write, and the
  oscillator itself all verified ticking correctly. See `scanner-uno/src/clock.h` for a real
  limitation this swap costs: DS1302 has no hardware power-loss flag equivalent to DS3231's OSF, so
  the "refuse taps without a trustworthy clock" rule is weaker here than the design elsewhere in
  this project.

A third swap happened at the uplink itself: **the original SIM900 shield was scrapped for a bare
SIM800L**, after it never once responded to `AT` across an exhaustive test matrix — every common
baud rate (9600/19200/38400/57600/115200), both TX/RX orientations, both power configurations
(external adapter and Arduino-supplied), confirmed ground, confirmed the module's own Status LED
lit (meaning it had power), and independent confirmation via a direct USB-UART adapter bypassing
the Uno entirely. The SIM800L, wired to the identical pins, answered `AT` -> `OK` at 9600 baud
within minutes once a separate 5V rail glitch was fixed.

```
   ┌───────────────────────────────────────┐
   │     Arduino Uno scanner (mains)        │
   │  RC522 (SPI)  ── card UID              │
   │  DS1302 (3-wire) ── timestamp          │
   │  LCD (I2C, PCF8574 backpack)           │
   │  Buzzer (GPIO) ── tap feedback         │
   │  SIM800L (SoftwareSerial, 9600)        │
   │    ├─ GPRS HTTP → same ingest/         │
   │    │   attendance endpoint as ESP32    │
   │    └─ SMS → same parent notify path    │
   │  EEPROM ── offline tap queue (~1 KB)   │
   └───────────────┬─────────────────────────┘
                    │  one module, one uplink, no Wi-Fi credential to manage
                    ▼
        same FastAPI / Supabase backend, same `attendance` table,
        same idempotent tap id — the server can't tell which MCU sent it
```

**The key difference from the ESP32 design isn't the MCU, it's the uplink count.** The ESP32
scanner carries two: Wi-Fi for the record, a SIM900 for SMS only (§Appendix: hardware — the ESP32
scanner's own SIM900 is unaffected by any of the above; that's a different board). Here the SIM800L
does both jobs itself — GPRS for the HTTP POST, SMS on the same AT session — so the scanner stops
depending on school Wi-Fi entirely. The tradeoff is total dependence on 2G coverage at the gate;
confirm the SIM800L actually registers on Globe or Smart's 2G band at the real deployment site
before committing, since 2G sunset dates vary by carrier and region in PH.

### Constraints that don't exist on the ESP32 build

| Constraint | Why it matters | What to do about it |
|---|---|---|
| **2 KB SRAM total** | RC522 + DS1302 + LCD + an AT-command parser + HTTP response buffer, all at once, on a chip with no heap headroom | No ArduinoJson — hand-build fixed-format request bodies as plain strings, parse only the HTTP status line back |
| **~30 KB usable flash** | RC522/DS1302/LCD libraries plus a hand-rolled SIM800L AT driver | Keep the AT driver to the exact command set needed (registration check, HTTP GET/POST, SMS send) — not a general-purpose GSM library |
| **One hardware UART, shared with USB/debug** | Unlike the ESP32's dedicated UART2 for the modem | SIM800L goes on `SoftwareSerial` at 9600 — confirmed against real hardware, not assumed |
| **No filesystem** | LittleFS doesn't exist on AVR | Offline tap queue lives in internal EEPROM — figure ~1 KB, i.e. a few dozen buffered taps, not the ESP32 scanner's deeper buffer. Fine for a gate with intermittent coverage, not for extended multi-day outages |
| **No OTA** | Uno has no wireless update path | Firmware updates require physical USB access — acceptable for a mains-powered, fixed station |

### Pins (fixed by the AVR, not remappable like ESP32 GPIOs)

| Signal | Uno pin | Notes |
|---|---|---|
| RC522 SCK/MOSI/MISO | 13 / 11 / 12 | Hardware SPI, not optional |
| RC522 SS / RST | 10 / 9 | Any free digital pin |
| LCD SDA/SCL | A4 / A5 | Hardware I2C, not optional — PCF8574 backpack, address `0x27` |
| DS1302 CE/IO/SCLK | 4 / 3 / 2 | 3-wire, NOT I2C — own dedicated pins, unrelated to the LCD's bus |
| Buzzer | 6 | Plain GPIO `tone()`, no transistor needed for a small piezo |
| SIM800L RX/TX | 7 / 8 (`SoftwareSerial`) | Level-shift Uno's 5 V TX into SIM800L's RX — confirmed not 5 V tolerant, unlike the SIM900 shield this replaced, which had its own onboard regulation |
| SIM800L power | Buck converter, 3.4-4.4 V | **Never the Uno's 5 V pin** — same 2 A burst warning as the tracker's SIM800L, see Appendix. A bulk capacitor (1000-2200uF) close to the module is not optional; without one the symptom is random resets that look like a firmware bug |

### Status

Firmware at `scanner-uno/` builds clean: RAM 1453/2048 bytes (70.9%), flash 30998/32256 bytes
(96.1%) — real numbers from `pio run -e uno` (flash headroom is now the tighter of the two, ~1.3KB
free, after the offline-fallback card feature below; RAM had come down from 1628 bytes (79.5%) via a
deliberate RAM-headroom pass, both changes verified booting cleanly on real hardware (full
RC522+DS1302+LCD+buzzer+SIM800L harness) before and after: `Wire`/`twi.c`'s I2C buffers overridden
32->16 bytes (`platformio.ini`'s `TWI_BUFFER_LENGTH` build flag plus a project-local `lib/Wire/`
copy for the one constant that isn't `#ifndef`-guarded upstream — saved 80 bytes, zero functional
risk since nothing here sends more than a few bytes per I2C transaction), and the SMS queue depth
cut from 2 to 1 (saved 123 bytes — safe because SMS here is explicitly best-effort, never the
durable record, so a second queued message just waits one more relay poll cycle).

Both the flash budget and this whole exercise trace back to a real lesson learned the hard way: an
earlier self-test build sat at 94.6% static RAM and produced a reset loop that looked exactly like
a hardware fault, but was actually RAM/stack exhaustion in the test harness itself — confirmed by
direct A/B hardware test (same wiring, low-RAM sketch, zero resets). The same class of bug hit the
real firmware too (a crash inside `display::begin()` that needed the *entire* harness connected to
reproduce, which made it look hardware-related until a plain RAM trim fixed it). The per-peripheral
isolation sketches (`scanner-uno/selftest_i2c/`, `selftest_rfid/`, `selftest_rtc/`,
`selftest_sim_only/`, `selftest_buzzer/`, `selftest_eeprom/`) exist because of that lesson — trust
their results over the combined `selftest/`'s when they disagree.

**Confirmed working on real hardware, this session:** RC522 (independent dump-sketch test), LCD
(visually confirmed text on screen), DS1302 (read/write/oscillator round-tripped correctly), EEPROM
(write/read verified), and SIM800L end to end — not just `AT` → `OK`, but SIM detected
(`+CPIN: READY`), registered on the home network (`AT+CREG` stat=1), and a real SMS sent and
delivered (`scanner-uno/selftest_sim_only/` is now a dedicated, reusable SIM800L test tool covering
all of this). The full integrated firmware (all six peripherals together) boots clean and reports
`modem=ok`. The one thing that turned out to matter as much as any wiring diagram: **a stable 5V
rail** — an unstabilized supply produced total AT-command silence on both the SIM900 and the
SIM800L that was indistinguishable from a dead module, until traced to the actual power glitch. A
second, unrelated real fault also turned up this way: the SIM800L's own SIM card wasn't making
contact (a common cause when a nano/micro SIM is riding in an adapter to fit the module's full-size
slot) — confirmed by the SIM working fine in a phone while the module reported `AT+CPIN? ERROR`.

**Update (2026-08-31): GPRS is now a closed question for this hardware, not an open one.**
Registration itself was checked (`AT+CREG?` → `stat=1`, real if weak signal `AT+CSQ ~3`) and GPRS
was re-tested end to end: `AT+CGATT=1` fails immediately, before the APN is even used. Root-caused,
not just observed — three independent signals agree: the SIM's data plan itself isn't the blocker
(mobile data confirmed working on a phone with this same SIM), that phone's network-mode settings
don't even offer a 2G-only option anymore, and the Philippines' NTC has a mandated nationwide 2G/3G
shutdown in progress (area-by-area, complete by 2026-12-31; Smart already dropped 3G in September
2025 — [NTC memorandum via CSA Group](https://www.csagroup.org/global-certification-regulatory-update/ntc-issues-memorandum-circular-on-2g-3g-network-shutdown-and-device-certification-restrictions/),
[Manila Times](https://www.manilatimes.net/2025/09/22/business/sunday-business-it/dict-3g-networks-to-shut-down-in-the-philippines-by-2026/2187832)).
The SIM800L is 2G-only hardware with no 3G/4G fallback, so this isn't fixable by config, APN, or
firmware changes — GPRS on this specific module is not a "confirm later" gap, it's a hardware
ceiling. **SMS-only is the durable design for this board, not a stopgap** — see
`scanner-uno/sms_scanner/`, the long-term firmware. A real HTTP/ingest path on Uno hardware would
need a 3G/4G-capable cellular module instead.

The plain-HTTP relay bridge this needs (the SIM800L can't speak TLS — see config.h) exists at
`relay-bridge/` — a small nginx reverse proxy holding no secrets, verified end to end against a
real HTTPS host during development (see its README for what was and wasn't tested). It still needs
a real Supabase project to point at, and a box with a public IP/domain to run on — the SIM800L
reaches it over cellular data, not the school LAN, which rules out "just run it locally" on a
machine behind NAT.

Known limitations of this first pass, worth reading before extending it: modem calls block the
caller for one AT exchange (a tap during a drain/relay call waits, bounded, rather than being
picked up instantly — see modem.h), the JSON parsing is hand-rolled for the exact shapes this
build controls (no escape handling), and roster/queue capacity (40 cards / 60 buffered taps) fits
a pilot, not a full school — see the EEPROM upgrade path above.

### Offline-fallback RFID card format

The normal attendance path never needs anything stored on the card beyond its UID — the roster
hash cache (`roster.cpp`) and server-driven SMS (`relay.cpp`) handle everything. But when the
network really is down (see the GPRS/signal issue above) and there's no way to reach the outbox
relay, the scanner has no way to know who to text. `scanner-uno/src/card.h` adds a second,
optional data payload written to each card for exactly that case: the parent's phone number and
the student's identity, read directly off the card and texted straight from the device with no
server round trip.

**Wire format** (48 bytes, one MIFARE Classic sector — sector 1, blocks 4/5/6; block 7 is that
sector's key trailer, not data): 1-byte magic, 1-byte version, 5 bytes packed BCD phone (10 PH
mobile digits, firmware prepends `+63`), 2-byte little-endian student ID, 20-byte ASCII name,
1-byte CRC8 over everything before it, 18 bytes reserved. Fitting in one sector is the actual
speed optimization the "compressed enough to be fast" requirement was about: one `PCD_Authenticate`
plus three sequential block reads, no second AUTH round trip. Protected by a project-specific key
(`CARD_KEY_A` in `include/config.h`) rather than the MIFARE factory default — a deterrent against
casual reads with a generic reader, not real security (MIFARE Classic's Crypto1 cipher is publicly
broken). This is also the honest privacy tradeoff of this design: a lost/stolen *card* now exposes
one child's contact info, whereas a lost *scanner* (which never stores this) still exposes nothing.

**Firmware integration:** `reader.cpp`'s `poll()` used to halt the card immediately on a successful
read; it now leaves the card selected and exposes a separate `release()`, so `card.cpp`'s
`card::read()` can pull the extra sector from the *same* tap session before the caller finally
calls `release()`. `main.cpp`'s tap handler calls `card::read()` only when `store::push()` succeeded
and `net::online()` is false, and on success enqueues a direct SMS via `smsq::enqueue()` — the same
best-effort queue the gateway path already uses, just fed locally instead of by the server.

**Enrollment:** a PC can't drive an RC522 directly, so `scanner-uno/card_writer/` is a second,
standalone Uno sketch (same RC522 wiring as the scanner) that writes/reads this format over a
simple serial line protocol (`WRITE,<phone>,<id>,<name>`, `READ`), verifying every write by reading
it back before reporting success. `scanner-uno/card_writer/card_writer_gui.py` is the enrollment
GUI (Tkinter + pyserial) office staff actually use: pick the serial port, fill in phone/ID/name,
tap Write, hold the card on the reader. Both the writer sketch and `card.cpp` hold their own copy
of `CARD_KEY_A` and the CRC8 algorithm — they have to match byte-for-byte or writes and reads
silently disagree; there's a comment at each copy saying so.

Not yet done: `CARD_KEY_A` is currently a placeholder ASCII-derived key (spells "ESPTRK"), not a
randomly generated one — fine for a demo, worth regenerating before any real deployment. The write
protocol also has no retry/backoff of its own; the GUI just reports the Arduino's error code
(`NO_CARD`, `AUTH_FAIL`, `WRITE_FAIL`, `VERIFY_MISMATCH`, ...) and expects the operator to retry by
hand, which is fine for a supervised enrollment desk but wouldn't scale to unattended use.

**Status: write + read verified end to end on real hardware**, including the scanner's own
`card::read()` — not a reimplementation of it. `card_writer` wrote a card
(`+639171234567` / id 7 / "WupaTest"), then `selftest_card/` (a new standalone sketch, same pattern
as the other `selftest_*/` folders, built by copying `card.cpp`/`reader.cpp`/`card.h`/`reader.h`
verbatim from `../src/` rather than re-deriving the logic) read it back on the same physical Uno
and printed exactly that data — proof the writer and the scanner genuinely agree on the wire
format, keys, and CRC, not just that each independently believes it does. Getting there took three
real bugs, all
found only by testing against physical cards rather than by reading the library source — the same
lesson as §1b's RAM-margin saga, just for RFID this time:

- **No key fallback on first write.** The very first version only ever tried `CARD_KEY_A` — but a
  factory-fresh MIFARE card ships keyed `FFFFFFFFFFFF`, so it could never authenticate a genuinely
  blank card, only one already written by this same program. Fixed by trying `CARD_KEY_A` first,
  then the factory default, and always rewriting the sector trailer to `CARD_KEY_A` afterward
  (transport-default access bits, only the keys change) so the card authenticates directly next
  time.
- **A failed key attempt poisons the next one.** Once the fallback above existed, it *still* failed
  against a genuinely blank card — `PCD_StopCrypto1()` between the two key attempts wasn't enough
  to recover the MFRC522's crypto engine after the first (expected) failure. Confirmed by a stock
  single-key MFRC522 example sketch, which never fails once and always succeeds, against our
  two-key version, which failed its second attempt every time. Fixed with a full halt + re-wake +
  re-select (`reselectCard()`) between candidate keys, not just `StopCrypto1()`.
- **REQA doesn't wake a halted card.** `PICC_IsNewCardPresent()` sends REQA, which per ISO 14443-3
  a halted card ignores — only WUPA (or physically leaving and re-entering the RF field) wakes it.
  Every command halts the card when it's done (`releaseCard()`), so with the same physical card
  never lifted between a write and a read (the natural way to test at a desk), the read's
  card-detection step never saw it again — `ERR:NO_CARD` every time despite the card visibly
  sitting on the reader. Fixed by switching `waitForCard()` to `PICC_WakeupA()`, which wakes a card
  from either state. (This distinction doesn't matter for the scanner's own `reader.cpp` — there, a
  continuously-held card simply not re-triggering until lifted is the *correct* behavior for
  attendance taps, so that file was left alone.)

---

## 2. Phase 1 — SOS pipeline (build this first)

### 2.1 Button handling
- **2 s hold, not a tap.** A tap in a pocket is a false alarm; enough of those and parents stop
  trusting the alert.
- Debounce in hardware (RC) *and* software. Recess the button so a backpack strap can't press it.
- On press: **immediate confirmation**, so the child knows it registered.
- On server ack: **a second, distinct pattern** — "your parent has been told." This is the detail
  most easily cut and least worth cutting.
- **With the LED standing in for the motor, the ack cue repeats for ~20 s.** A buzz is felt
  through a pocket; an LED the child is not looking at is a cue that never arrived. Revert to a
  one-shot when the motor is stocked.
- **5 s cancel window** via a second long-press, for accidents. After that, no cancel.

### 2.2 Transmission — the five-second rule
```
SOS pressed
  ├─ haptic confirm
  ├─ start all position sources in parallel, 30 s deadline:
  │    ├─ BLE anchor scan   (~1 s, if in a known place)
  │    ├─ cell tower ID     (~2 s, always available if attached)
  │    ├─ Wi-Fi AP scan     (~4 s, the indoor winner)
  │    └─ GNSS warm fix     (~15 s outdoors; NEVER COMPLETES INDOORS)
  ├─ AT t+5 s: TRANSMIT with the best fix so far  ← do not wait for GNSS
  ├─ send refined fixes as they arrive
  └─ enter high-rate mode: position every 30 s for 30 min
```
The deadline is not an optimisation, it is what makes the device work indoors at all. Indoors the
GNSS branch never returns; a design that waits for it waits forever. "SOS pressed, position
approximate" at six seconds beats a perfect fix at forty-five, because the parent is already
moving while the device is still refining.

- **Store-and-forward queue in flash.** If the modem can't attach, persist the event with its
  *original* timestamp and retry with exponential backoff, forever. Never drop an SOS.
- **The HTTP 200 is the ack.** The device releases an event from its flash queue on 200 and on
  nothing else. Every event carries an id, so a retried POST over a flaky 2G link is idempotent
  — a redelivery must never become a second position on the map.

### 2.3 Escalation ladder
Fan-out is not a design. Escalate until a human acknowledges:
```
t+0s    Push to all parent devices + WebSocket to open dashboards + SMS
        ^ the server SKIPS this SMS when the device reports device_sms_sent,
          because the tracker already texted the parent directly over 2G.
          One press must never arrive as two texts.
t+60s   No ack → SMS to secondary contact
t+180s  No ack → automated voice call to primary
t+300s  No ack → voice call to secondary
```
- Every message carries a **one-tap deep link to the live map**, never bare coordinates.
- `POST /api/sos/{id}/ack` halts the ladder at any point and records who answered.
- **Rate limit** 5 SOS/hour/device; suppressed events still raise a distinct "possible stuck
  button" operator alert.
- **Drill mode:** a clearly-labelled test SOS. Practise it with the child. An untested panic
  button is a decoration.

### 2.4 Acceptance criteria
- [ ] p95 press → parent notification ≤ 20 s over 50 real presses
- [ ] 100% delivery across 20 presses in a cellular dead zone (queued, delivered on reattach)
- [ ] **20 presses from inside a school/mall/home deliver a usable position** (≤ 50 m)
- [ ] 0 false SOS in 14 days of a child actually wearing it
- [ ] The child can describe what the two confirmation patterns mean, unprompted

---

## 3. Position: the indoor problem

### 3.1 What actually works where

| Environment | GNSS | Wi-Fi scan | BLE anchor | Cell ID | Result |
|---|---|---|---|---|---|
| Outdoors, open sky | 5–15 m | — | — | — | Excellent |
| Urban street | 10–30 m | 15–40 m | — | — | Good |
| Home / apartment | poor–none | 10–30 m | exact place | — | Good |
| School, office | none | 15–40 m | exact place | — | Good |
| Mall, transit hub | none | 20–50 m | — | — | Usable |
| Parking garage, basement | none | sparse | — | 100 m–2 km | **Weak** |
| Rural indoors | none | no APs in DB | exact place | 1–5 km | **Poor** |

GNSS signal is around −130 dBm outdoors and buildings attenuate it 10–30 dB. Near a window in a
wood-frame house you may get a degraded fix; through concrete and steel you get nothing.
**A-GNSS does not solve this** — assistance data cuts time-to-first-fix, it does not improve
sensitivity. If the antenna cannot see satellites, assistance conjures nothing.

**The uplink, by contrast, survives indoors.** LTE-M and especially NB-IoT were designed for
basement utility meters; NB-IoT's coverage-enhancement modes buy roughly 20 dB of link budget
over GSM. The SOS and the offline heartbeat keep working in places where a phone shows one bar.
This is the main argument for LTE-M over a plain LTE modem.

### 3.2 Wi-Fi scanning is the indoor primary, not a fallback
Passive BSSID + RSSI scan — no association with any AP — resolved to coordinates by a
geolocation provider. Typically 10–40 m in built-up areas, frequently *better* than a struggling
GNSS fix. This is how phones locate you inside a mall.

Providers to evaluate: **Unwired Labs** (free tier, good for the prototype), **Combain**,
**LocationIQ**, **Google Geolocation API** (~$5/1000 requests). Test accuracy on the child's
actual routes before committing — coverage of the AP database is what matters, and it varies
enormously by country.

### 3.3 Known-place anchoring — named by the parent, from the dashboard

The tracker sends its Wi-Fi scans (BSSID + SSID + RSSI, capped at ~6 APs) with each report. The
server hashes the BSSIDs, keeps seven days of scans, and the dashboard shows the parent **what
their child's tracker can actually see** — by SSID, with how often each network appeared this
week. The parent ticks the ones that are always there, types "School", and saves.

That was the missing link in v3: the schema had a `bssid_set` column and no way on earth to fill
it. Asking a parent to type in MAC addresses was never going to happen; showing them
`DepEd-Classroom-3` and letting them tick it does.

A named place then buys three things at once:

- **No API call at all** in the places the child spends 90% of their time — matching runs
  on-device against the stored hashes.
- **The dashboard says "At school since 08:41"** instead of plotting a 30 m circle. Indoors,
  place semantics are what a parent actually wants; coordinates are the worse answer.
- **The provider never learns the child's routine.** Streaming BSSID lists to a third party leaks
  the child's surroundings to exactly the sort of company we self-hosted this system to avoid.
  Anchoring means the API only ever sees unfamiliar places.

Cache resolved scans on-device keyed by BSSID set, and only call out when the set materially
changes. A 2-minute cadence while moving gets expensive fast otherwise.

**Store BSSIDs hashed, SSIDs in clear.** An AP-level history is a finer-grained record of where a
child has been than the coordinates are, so a database dump must not hand one over. The SSID is
the only part a parent can recognise, and matching runs on the hashes regardless.

### 3.4 BLE anchors for the places that matter
A ~$3 beacon by the front door and in the classroom gives *certainty* where Wi-Fi geolocation
gives an estimate — for the two or three locations that matter most. Note the direction of
travel: **the tracker scans, it does not advertise.** This adds no unwanted-tracker exposure.

### 3.5 Where it genuinely fails
Underground parking, and rural interiors whose single AP is in no database. Both fall through to
cell-tower ID: kilometres, not metres. There is no clean fix. The honest response is that the API
returns `source='cell'` with a truthful `accuracy_m`, and the UI draws a two-kilometre circle
rather than a confident-looking pin. **Never let the map imply precision the fix does not have.**

---

## 4. Phase 2 — Routine tracking and power

### 4.1 Report on state change, not on a timer
| State | Detected by | Cadence |
|---|---|---|
| Stationary (school, home) | Wi-Fi BSSID set unchanged (Jaccard ≥ 0.55) + same serving cell | 1 / 30 min |
| Moving | BSSID set changed, or serving cell changed | 1 / 2 min |
| **No usable signal** | No APs visible *and* no cell change (rural) | Fixed 1 / 10 min — never guess "stationary" |
| Geofence crossing | On-device polygon test | immediate |
| SOS active | Button | 1 / 30 s for 30 min |
| Low battery < 15% | Fuel gauge | alert, then 1 / hr |

**There is no accelerometer in the BOM**, so motion is inferred from the radios already present.
Three tiers, each one's cost matched to its confidence: the serving cell every 60 s (free — the
modem is awake anyway), a Wi-Fi scan every 2–5 min (~0.05 mAh, and the authority), and GNSS only
on demand. This is arguably a better signal than the part it replaces: an accelerometer reports
that the device is being *jostled*, which a child fidgeting at a desk does all day, whereas a
changed Wi-Fi neighbourhood reports that they are somewhere *else*. Cost of the substitution is
about 0.5% of the battery; what is lost is interrupt-driven wake, which matters little when the
modem outdraws the sleeping MCU by three orders of magnitude.

Geofences are evaluated **on-device**, so a "left school" alert never waits for the next upload.
Batch 2–3 hours of routine positions into one modem wake — sixty wakes for sixty points is the
single biggest avoidable power cost in the system.

### 4.2 Power budget (2000 mAh, target ≥ 5 days)
| Item | Est. draw |
|---|---|
| ESP32-S3 deep sleep | 20–40 µA |
| Modem PSM idle | 1–3 mA |
| Wi-Fi scan (passive, ~2 s) | ~80 mA burst — also the motion gate, ~13 mAh/day |
| BLE anchor scan (~1 s) | ~40 mA burst |
| GNSS fix (15 s, ~10×/day) | ~30 mA burst |
| Modem TX burst | 200–500 mA, ~2 s |

Measure with a PPK2 or INA219 — datasheet figures will not survive a real TLS handshake. Budget
for charging every 3–4 days. Push a low-battery alert at **20% and again at 10%**: a dead tracker
still showing its last pin is the worst failure this system can produce.

---

## 5. Data model (PostgreSQL; SQLite fine for the prototype)

```
devices           id, kind ENUM('tracker','scanner'), name, child_name,
                  token_hash, msisdn, created_at, active,
                  battery_pct, signal_csq, balance_pesos, last_seen_at, firmware
                  -- msisdn drives the "locate now" SMS downlink
                  -- balance_pesos: prepaid load is a silent killer, watch it

places            id, device_id, name, kind ENUM('home','school','other'),
                  bssid_set (hashed), ble_anchor_id, lat, lon, radius_m
                  -- powers "at school" instead of coordinates, and keeps
                  -- routine locations away from the geolocation provider

locations         id, device_id, lat, lon, accuracy_m, recorded_at, received_at,
                  source ENUM('gnss','wifi','ble_anchor','cell','manual'),
                  place_id NULL, battery_pct, speed_mps, heading
                  -- recorded_at ≠ received_at: store both, always
                  -- INDEX (device_id, recorded_at DESC)

sos_events        id, device_id, triggered_at, received_at, latency_ms,
                  first_location_id, best_location_id,
                  status ENUM('open','acknowledged','resolved','false_alarm'),
                  acknowledged_by, acknowledged_at, resolved_at, notes, is_drill

alerts            id, sos_event_id, channel ENUM('push','sms','voice','email'),
                  recipient, sent_at, provider_msg_id, delivery_status, error
                  -- audit trail: proves what actually reached whom

attendance        id, event_id UNIQUE, scanner_id, card_uid, child_device_id,
                  direction ENUM('in','out'), recorded_at, received_at

cards             card_uid PK, device_id, child_name, active
                  -- UID -> child mapping lives ONLY here. A stolen scanner must
                  -- not reveal who attends the school.

geofences         id, device_id, name, polygon|centre+radius, alert_on ENUM('enter','exit','both')
geofence_events   id, geofence_id, device_id, location_id, direction, occurred_at

device_health     id, device_id, reported_at, battery_pct, rssi, network_type,
                  uptime_s, queue_depth, last_reset_reason

users             id, email, password_hash (argon2id), role, totp_secret
sessions          id, user_id, token_hash, expires_at, revoked_at
device_access     user_id, device_id     -- explicit grants, no implicit "sees everything"

integration_health  id, integration, last_success_at, last_error, status
                    -- geolocation provider, SMS provider
```

**Retention:** purge raw positions older than 90 days; downsample to hourly beyond 30 days.
Keep `sos_events` indefinitely. Make it configurable and *make sure the job actually runs* — this
is a database of a child's movements, and every extra day of history is extra liability.

---

## 6. Consent and legal

Dropping FMDN removes the anti-stalking problem, but not the human ones.

1. **No unwanted-tracker exposure.** The device advertises nothing and scans passively, so it
   trips no "unknown tracker travelling with you" detection. Keep it that way: any BLE
   advertising for pairing or find-mode must be **opt-in, short-window, and never persistent**.
2. **Tell the child.** A tracker a child knows about is a safety tool; one they discover is
   surveillance, and the discovery destroys trust irreparably. Age-appropriate disclosure, and
   let them see their own map.
3. **Consent of the other parent or guardians.** In shared-custody situations, unilateral
   tracking has been treated as harassment in some jurisdictions. Get agreement in writing.
4. **Jurisdiction.** Rules on tracking minors and on data retention vary. If this becomes a
   deliverable for a client (this repo lives under `possible-client/`), get real legal review
   before anyone else's child wears it. That is a different risk tier than your own.

---

## 7. Security

A live database of a child's real-time position is one of the highest-value targets a
self-hosted server can carry.

**Device → server**
- Per-device X.509 client cert (mTLS) or a unique rotatable token. **No shared fleet secret** —
  one recovered device must not compromise the others.
- Credentials in **encrypted NVS** (ESP32 flash encryption + secure boot enabled).
- **Signed OTA with rollback.** An unsigned OTA endpoint is remote code execution on a device
  attached to a child.
- Server-side sanity checks: timestamp skew, plausible lat/lon, plausible speed between
  consecutive fixes (reject 900 km/h jumps).

**Server → parent**
- No unauthenticated endpoints. Ever. Not "it's only on my LAN."
- Argon2id hashing; mandatory TOTP 2FA on parent accounts.
- Short-lived tokens with real revocation; `device_access` enforced in **every** query — never
  trust a client-supplied `device_id`.
- HTTPS only (Caddy/Traefik auto-TLS), HSTS, Secure + HttpOnly + SameSite cookies.
- Rate limiting on auth and on history endpoints.
- Audit log of who viewed which child's location, and when.

**Third parties**
- Geolocation API key server-side only, never on the device or in the frontend. The device sends
  BSSID lists to *your* server; your server talks to the provider.
- Minimise what leaves: known-place anchoring (§3.3) means routine movement never reaches the
  provider at all.
- Wi-Fi scans are retained **7 days, with BSSIDs hashed**. Keep the retention job honest: an
  AP-level trail is more revealing than the coordinate history sitting next to it.

**Infrastructure**
- Encrypted volumes; encrypted, off-site, **restore-tested** backups.
- No port-forwarding from a home router — VPS, or Tailscale/WireGuard.
- Dependency scanning in CI.

---

## 8. Phase 3 — API and dashboard

```
POST   /api/auth/login                    → token (+ TOTP)
POST   /api/auth/refresh
GET    /api/devices                       → last position per device, WITH source + recorded_at
GET    /api/devices/{id}                  → detail, health, battery
GET    /api/devices/{id}/history?from&to&source&limit
GET    /api/devices/{id}/wifi             → what the tracker sees: SSIDs, how often, already named?
POST   /api/devices/{id}/places           → parent names a set of networks ("School")
DELETE /api/places/{id}
GET    /api/devices/{id}/geofences
POST   /api/devices/{id}/geofences
GET    /api/sos                           → open + recent
POST   /api/sos/{id}/ack                  → halts escalation
POST   /api/sos/{id}/resolve
GET    /api/attendance                    → scanner taps
GET    /api/health/integrations           → geolocation + SMS provider, device freshness
WS     /ws/live                           → real-time position, SOS, taps
POST   /api/devices/{id}/locate           → "locate now": SMS wakes the device
POST   /api/ingest/events                 → tracker, batched (200 = ack)
POST   /api/ingest/taps                   → scanner, batched (200 = ack)
```

`/api/devices` **must** return `source`, `accuracy_m`, `recorded_at` and `place` alongside the
coordinates. Conflating a four-second GNSS fix with a two-kilometre cell estimate is the most
dangerous bug available in this system.

**Frontend** — Leaflet + OpenStreetMap (check the OSM tile policy for anything beyond personal
use; budget for Protomaps/MapTiler if this becomes a product).
- **SOS state dominates the UI:** full-screen banner, audible alarm, one-tap acknowledge. A parent
  should understand the situation from across the room.
- **Show the place, not the pin, when there is one.** "At school since 08:41" beats a circle.
- Pin styling by `source`; accuracy ring sized by `accuracy_m`; **age badge on every pin**;
  desaturate past 15 minutes with "last seen 23 min ago".
- History trail with a time scrubber; dashed segments across coverage gaps.
- Health strip: battery %, signal, network type, integration status.
- Ship as a **PWA** with web push so the phone alerts with the tab closed.
- Design the offline, empty and error states rather than defaulting them.

---

## 8b. Deployment

Two backends share this schema and these contracts.

**Path A — Supabase + Vercel (production).** Postgres with RLS, Supabase Auth, Realtime, and the
escalation ladder as `alert_queue` rows drained by `pg_cron` every minute. No always-on server.

**Path B — FastAPI (local dev, reference).** The same system in one readable Python service. Best
for development, since `SMS_PROVIDER=console` exercises the whole ladder without sending anything.

### What serverless forced, and why each change is an improvement

| Was | Now | Gain |
|---|---|---|
| asyncio ladder sleeping 300 s | `alert_queue` rows + pg_cron | The asyncio version lost every pending escalation on restart; rows survive |
| WebSocket hub | Supabase Realtime | RLS applies to the stream, so a parent is only pushed their own devices |
| argon2 + TOTP + sessions | Supabase Auth | RLS policies get `auth.uid()` free; MFA built in |

A Vercel function is killed the moment it returns, so a coroutine sleeping five minutes cannot
exist. **Rung 1 is therefore not queued** — it fires synchronously inside the ingest request, so
the first alert never waits for a scheduler. Only the secondary SMS and the two voice calls go
through the queue, where cron's ±60 s does not matter.

### RLS is the entire security model

Supabase exposes every table over PostgREST using the anon key, **which ships in the frontend
bundle**. A table without RLS enabled is readable by anyone who opens devtools. For a live
database of a child's location that is not a misconfiguration; it is the worst outcome available
here.

`0002_rls.sql` enables RLS on all fourteen tables with policies routed through `device_access`.
`alert_queue`, `access_log` and `integration_health` get **no policies at all** — RLS on with
nothing matching makes them invisible to everyone but the service role. The service role key
bypasses RLS entirely and belongs only in `supabase secrets` and Vercel environment variables.

## 9. Build order

| Phase | Deliverable | Gate |
|---|---|---|
| **0** | Verify LTE-M/NB-IoT coverage *and* Wi-Fi geolocation accuracy on the child's real routes, indoors and out | Attach confirmed on the school run; Wi-Fi resolves to ≤ 50 m inside the school |
| **1** | SOS end-to-end: button → cellular → dispatcher → SMS/push/voice with ack | §2.4 criteria pass, including the indoor test |
| **2** | Layered position fusion, known-place anchoring, BLE anchors, adaptive cadence, geofences | ≥ 5 days runtime; correct place detection at home and school for a week |
| **3** | Schema + RLS on Supabase, Edge Functions, Vercel dashboard | §7 checklist complete; **every table shows RLS enabled**; `cron.job` lists both jobs |
| **4** | Enclosure, field trial, SOS drills | 30-day trial, no missed SOS; child explains the confirmation patterns unprompted |

Phase 0 is the only real go/no-go in the project, and it now has **two** gates: the uplink and the
indoor fix. Both are cheap to test with a bare module and a weekend, and both are expensive to
discover in Phase 3.

---

## 10. Failure modes

Every one has the same shape: the system stops knowing where the child is, and the UI keeps
showing the last pin as though nothing happened. Each needs an **active alert**, not a grey dot.

| Failure | Detected by | Response |
|---|---|---|
| Battery dead | No telemetry for 2× expected interval | Push "tracker offline since 14:20" |
| No cell coverage | Modem detach | Queue in flash; show last known + true age |
| No position source (garage, rural indoors) | All four sources fail or return cell-only | Report `source='cell'` with honest radius; never fake precision |
| Geolocation provider down or over quota | API 4xx/5xx | Fall back to anchors + cell; operator alert; positions still flow |
| **Prepaid load exhausted** | GPRS attach fails; balance poll | Alert the parent BEFORE zero. In PH this is a routine, silent outage |
| **SIM deregistered** (RA 11934) | Network registration refused | Operator alert — it looks exactly like a hardware fault |
| Scanner offline (school Wi-Fi) | No taps + no heartbeat | Taps still accepted and buffered with RTC time; alert the operator |
| Server down | External uptime monitor | Page the operator; device queues and replays |
| Stuck button | > 5 SOS/hour | Rate-limit + distinct operator alert |
| Device left behind | No motion at a known place for hours | Optional "possibly not with the child" notice |
| Device lost or stolen | — | Revoke device cert; remote key wipe |
| Clock skew on device | Server-side timestamp check | Trust `received_at`; flag the record |

---

## 11. Open questions before Phase 0

1. ~~Country and carrier~~ — **answered: Philippines.** 2G is widely deployed on Globe and Smart;
   LTE-M/NB-IoT effectively are not. SIM800L is the correct part, not a compromise. Still verify
   `AT+CREG?` and `AT+CSQ` on the child's real route, on **both** carriers — regional coverage
   differs a lot. Note that carriers have signalled an eventual 2G sunset with no firm date.
2. **Wi-Fi geolocation coverage in PH** — how good is the AP database around the school and home?
   Test with a phone-collected scan before committing to a provider.
2b. **Prepaid or IoT plan?** Load expiry is a silent outage. Poll the balance (`*143#` Globe,
   `*123#` Smart) and register the SIM under RA 11934 or it gets deactivated.
3. Child's age, and whether they already carry a phone — a phone changes the calculus and may
   make this a companion device rather than the primary one.
4. Own child, or a deliverable for the client this repo is named after? The second answer adds
   legal review, liability, and support obligations.
5. Wearable, backpack-clipped, or pocket? Drives enclosure, battery size, button placement — and
   Wi-Fi/GNSS antenna performance more than people expect.
6. SMS and voice budget and provider — voice escalation costs real money.
7. VPS or hardware at home? (See §7 — don't port-forward.)

---

## Appendix: hardware

### Tracker (worn)
| Role | Part | Why |
|---|---|---|
| Compute | ESP32 (WROOM-32) | Supplies Wi-Fi + BLE scanning itself — two of the four position sources, free |
| Uplink | **SIM800L** | 2G/GSM. Correct for PH; LTE-M/NB-IoT are not available here |
| GNSS | **NEO-6M** | Keep V_BCKP powered or every fix is a 27 s cold start |
| Power | LiPo 2500–3000 mAh + TP4056 | Bigger than v3's 2000 mAh — see below |
| Bulk cap | **1000–2200 µF, close to the SIM800L** | Not optional |
| Trigger | Recessed tactile button | RC debounce in hardware; RTC-capable GPIO for deep-sleep wake |
| Feedback | **LED** *(motor out of stock)* | Same GPIO as the future motor. Ack cue repeats 20 s because an LED must be looked at, not felt. A piezo buzzer would be a closer substitute |
| Motion | *(none — no accelerometer stocked)* | Inferred from Wi-Fi + cell; see §4.1 |
| Known places | 2–3 BLE beacons | ~$3 each; certainty at home and school, zero API cost |

**SIM800L power is the number one build risk.** It draws up to **2 A** in transmit bursts at
3.4–4.4 V. It cannot run from the ESP32's 3V3 pin or an AMS1117 — wire it **directly to the
LiPo** with a large bulk capacitor physically close to the module and thick, short wires.
Missing that capacitor presents as random ESP32 resets that look exactly like firmware bugs.
Level-shift the ESP32 TX line; SIM800L RX is not 3.3 V tolerant.

**Battery regression:** SIM800L has no LTE-M power-saving mode. Expect **2–3 days** rather than
five. Biggest lever is `AT+CSCLK=1` with DTR wake; without it, idle draw is 10–20 mA. Keep the
modem **attached and sleeping** rather than powered down — a cold GSM attach costs 5–15 s, and
for a safety device latency beats runtime.

### Scanner (school gate, mains-powered)
| Role | Part | Why |
|---|---|---|
| Compute | ESP32 | Wi-Fi uplink; no power budget to worry about |
| Reader | **RC522** | 13.56 MHz. **3.3 V only — 5 V destroys it** |
| Clock | **DS3231 RTC** | Non-optional: a buffered tap with a wrong timestamp silently corrupts the attendance record |
| Feedback | Buzzer + green/red LED | The child must know the tap took |

The scanner reads the card **UID**, which is trivially cloneable. That is fine for attendance —
the failure mode is a wrong record, not an open door — and **not** acceptable for access
control. Do not let this scanner unlock anything.

## Appendix: dropped from earlier revisions

- **FMDN beacon firmware** (v1, v2) — see §0.
- **GoogleFindMyTools poller, headless login, session-cookie management, local blob
  decryption** (v1, v2) — removed with it.
- **Session-freshness indicator in the UI** (v1, v2) — replaced by `integration_health` covering
  the broker, the geolocation provider, and the SMS provider.
- **`source='fmdn'`** in the schema — replaced by `wifi`, `ble_anchor`, `cell`.
