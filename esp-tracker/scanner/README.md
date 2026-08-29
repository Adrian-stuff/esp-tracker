# Scanner — ESP32 + RC522 RFID

Fixed attendance station. The child taps their card, the scanner posts the event to the same
backend as the tracker, and the parent dashboard shows arrival and departure.

This is a **different project from the earlier "put the tracker on the school's reader" idea**,
and a much better one: because you own this reader, you own the tap event. No school API, no
vendor cooperation, no credential emulation.

---

## Bill of materials

| Role | Part | Notes |
|---|---|---|
| MCU | ESP32 DevKit v1 | Wi-Fi uplink; mains-powered, so no power budget to worry about |
| SMS | **SIM900** | Second uplink. Texts the parent even when school Wi-Fi is down |
| Reader | MFRC522 (RC522) | 13.56 MHz. **3.3 V only — 5 V destroys it** |
| Clock | DS3231 RTC | Non-optional, see §"Why the RTC" |
| Feedback | Buzzer + green/red LED | The child must know the tap took |
| Optional | SSD1306 OLED | Shows the name — turns a beep into a confirmation |
| Power | 5 V USB supply | Into VIN, not into the RC522 |

---

## Pinout

| Signal | GPIO | Bus |
|---|---|---|
| RC522 SDA/SS | 5 | SPI |
| RC522 SCK | 18 | SPI |
| RC522 MOSI | 23 | SPI |
| RC522 MISO | 19 | SPI |
| RC522 RST | 4 | moved off 22 to free I²C |
| RC522 3.3V | — | **3.3 V, never 5 V** |
| SIM900 TX / RX | 16 / 17 | UART2. Level-shift the ESP32 TX line |
| SIM900 PWRKEY | 32 | Hold low ~1.2 s to toggle |
| DS3231 / OLED SDA | 21 | I²C |
| DS3231 / OLED SCL | 22 | I²C |
| Buzzer | 25 | via transistor |
| LED green | 26 | |
| LED red | 27 | |

---

## Why the RTC is not optional

The ESP32 has no battery-backed clock. It gets the time from NTP over Wi-Fi.

School Wi-Fi will drop. When it does, the scanner must keep accepting taps and buffer them —
and a buffered tap with the wrong timestamp is worse than no tap at all, because it silently
corrupts the attendance record. The DS3231 keeps time across reboots and outages, so queued
events carry the moment they actually happened.

Same principle as the tracker: **`recorded_at` and `received_at` are different fields and both
get stored.**

## SMS — the second uplink

The SIM900 is not a nicety. Wi-Fi is the *record*; SMS is the *notification*, and they fail
independently. When the school network drops, taps still queue to flash **and** the parent still
gets a text — which is the whole reason the module is here.

**Sending is queued, never inline.** `AT+CMGS` is a multi-step exchange (command → `>` prompt →
body → Ctrl-Z → wait for OK) that takes 3–6 s on 2G. Doing that in the tap handler would stall the
reader for the whole exchange, and a gate is a queue of thirty children. So a tap enqueues and
returns immediately; `smsq::service()` advances one step per `loop()`.

**Messages are not retried forever.** Unlike tap records, they are dropped after two attempts. A
text saying "Ana tapped in" that arrives three hours late is worse than nothing — it reads as a
fresh event. The tap still reaches the dashboard by HTTPS, which is the durable record.

**One tap is never two texts.** The scanner reports `device_sms_sent` with each tap, and the
server skips its own message for those. If the SIM900 was unregistered, the flag is false and the
server sends instead — so the parent hears exactly once, by whichever path worked.

**Per-card cooldown of 5 minutes** (`SMS_PER_CARD_COOLDOWN_S`), separate from the 3-second reader
debounce. A child fiddling with their card must not become ten messages.

### Relaying the server's messages

Beyond its own taps, the scanner sends SMS **on the server's behalf**. Both modems are paid for
already, and on a matching PH network a device-sent text is effectively free where Semaphore
charges per message — so the server offers routine notifications to the devices first and only
pays if nobody delivers.

```
server enqueues       →  outbox(to, body, fallback_after)
scanner polls /outbox →  claims up to 2, lease 60 s
scanner sends, acks   →  POST /outbox/{id}/ack {sent:true}
nobody claimed in 90s →  server sends via the provider instead
```

The **lease** stops two devices sending the same message. The **fallback timer** stops a dead
scanner silently swallowing every notification — the failure this would otherwise introduce.

Three rules hold it together:

- **SOS never uses this path.** An emergency must not wait for a poll, a claim and a
  confirmation. The escalation ladder goes straight to the provider.
- **The scanner may relay anything; a tracker only its own child's messages.** The scanner is
  fixed, shared infrastructure. A tracker already holds its own parent's number for SOS, so
  relaying costs no new exposure — but a general relay would push other families' numbers onto a
  child's device.
- **Relayed numbers never touch flash.** They live in RAM for the duration of one send.

Acks are sent *before* polling for more work, because a claim the server still believes is
outstanding blocks that message from being retried or paid for until the lease expires.

### Direct mode, and where it stops being appropriate

This build runs `SMS_DIRECT_MODE = true`: parent numbers live in `config.h` and the scanner texts
them itself. **That is right for one family at their own door, and wrong at a shared school gate.**
There it would put every parent's phone number in flash on a box bolted to a wall, force a
firmware flash whenever anyone enrolls or changes number, and discard the reason the roster is
hashed in the first place. For that deployment, send the tap to a server gateway number and let
the server fan out.

### Power

Same rule as the tracker's SIM800L: **up to 2 A in transmit bursts at 3.4–4.4 V.** It cannot run
from the ESP32's 3V3 pin or straight off USB 5 V. Use a buck regulator to ~4 V (or a shield with
its own) plus a 1000–2200 µF bulk cap close to the module. Being mains powered removes the battery
budget, not the current spike.

## Direction: in or out?

**The scanner does not decide, and should not.** One reader cannot see which side of the gate a
child walked from, the device loses per-card state on reboot, and — the reason that actually
forces it — taps buffered during a Wi-Fi outage **arrive late and out of order**. A tap from 07:58
can land after one from 15:10. Anything deciding direction at arrival time gets those backwards.

So the server recomputes direction from the card's own history for that day, ordered by
`recorded_at`: odd taps are `in`, even are `out`. A database trigger reruns it on every insert,
and because the operation is idempotent, a late arrival **self-corrects** the whole day.

The dashboard reads `attendance_days`, which collapses that into what a parent actually wants —
first in, last out, tap count — rather than a list of raw scans.

## Unknown cards, without a roster on the device

The UID→child mapping lives only on the server, deliberately: a stolen scanner must not reveal
who attends the school. But that leaves the gate unable to tell an enrolled card from a
stranger's, so a child whose card was never registered gets a cheerful beep and nobody finds out
for weeks.

The compromise is `GET /functions/v1/roster`, which returns **salted hashes** — neither the names
nor the UIDs themselves leave the server. The scanner hashes the UID it reads with the same salt
and compares. It caches ~400 hashes in NVS (~1.6 KB) and refreshes every 6 hours.

`ROSTER_SALT` must match on both sides. It is not a secret from whoever holds the device; it only
stops a precomputed table of every 4-byte MIFARE UID.

Be honest about the limit: card UIDs are short and low-entropy, so these hashes are
brute-forceable by anyone holding the device. What they do **not** carry is identity — no names,
no classes, no parents. That is the whole claim, and it is still worth having.

An unrecognised card is **still queued**, flagged, and shown on the dashboard. A card tapping
repeatedly that nobody enrolled is exactly the thing worth noticing.

## Offline buffering

The scanner uses the same store-and-forward pattern as the tracker: an append-only LittleFS log
plus a read cursor, so a power cut mid-write costs one record rather than the whole queue. The log
compacts once the delivered prefix passes `QUEUE_COMPACT_AT`. Capacity is ~2000 taps, roughly two
days of a busy gate.

Taps are drained in batches of 50 so the TLS handshake is amortised, and a batch leaves the queue
**only on an HTTP 200**. Every tap carries a device-generated id, so a retried batch cannot
produce duplicate attendance records.

**The child's beep comes from the local queue write, not the server round trip.** Otherwise a
Wi-Fi outage looks to a child like a rejected card, and they walk off assuming they were not
recorded. A distinct cue tells them it was stored rather than sent.

## Feedback is audible first, and non-blocking

A gate is a queue of thirty children. The scaffold's `delay(FEEDBACK_HOLD_MS)` after each tap put
a hard ~1.3 s floor on throughput and made the reader feel broken under load, so cues are now
millis()-driven and the reader accepts the next card immediately.

Cues are distinguishable **by ear** — children at a gate are looking at each other, not at a box
on a post. Rising tones mean good, falling means not:

| Cue | Sound | Means |
|---|---|---|
| Accepted | short rising chirp, green | Enrolled card, queued |
| Offline | chirp then a lower note, amber | Stored, will send when Wi-Fi returns |
| Unknown | low double buzz, red | Card is not enrolled — still recorded |
| Duplicate | soft tick | Same card inside the debounce window |
| Error | long low tone, red | Could not store: no clock, or flash full |

Between taps the LEDs show ambient health: solid green = online and empty queue, slow amber =
buffering, fast red = **no usable clock**, which is the state that matters most.

## Getting on the school network

Worth resolving before you build the enclosure: many school networks use WPA2-Enterprise, a
captive portal, or MAC allowlisting, and none of those are things an ESP32 handles gracefully.
Ask the IT contact for a device registration on the IoT/guest VLAN. If that is refused, the
fallback is the scanner's own uplink — but at that point a BLE anchor by the door (already in
PLAN.md Phase 02) gives you the same arrival event with no reader and no network at all.

---

## Security: what this is and is not

The RC522 reads the **UID** of a MIFARE Classic card. UIDs are trivially cloneable with a phone
or a €5 "magic card."

That is **acceptable for attendance** — knowing a child tapped in is useful even if the mechanism
is spoofable, and the failure mode is a wrong attendance record, not an open door.

It is **not acceptable for access control.** Do not let this scanner unlock anything, and do not
let the attendance record become the basis of a security decision. If you later want real
assurance, that means DESFire with server-side key management, which is a substantially bigger
project.

Other rules that carry over from PLAN.md §7:
- The **card UID → child mapping lives on the server**, never on the device. A stolen scanner
  must not reveal who attends the school.
- The scanner authenticates to the backend with its own per-device credential, like any other
  device. No shared fleet secret.
- Rate-limit repeat taps of the same card (see `TAP_DEBOUNCE_MS`) so a card left on the reader
  does not generate hundreds of events.

---

## Build

```bash
pio run -t upload -e scanner && pio device monitor
```
