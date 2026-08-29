# Tracker — ESP32 + SIM800L + NEO-6M

The wearable device. One cellular uplink, four position sources, one SOS button.

Hardware locked to what's in stock: **SIM800L instead of SIM7080G.** That substitution has
three consequences that change the firmware design, documented below. Read them before wiring
anything.

---

## Bill of materials

| Role | Part | Notes |
|---|---|---|
| MCU | ESP32 DevKit v1 (WROOM-32) | Provides Wi-Fi scan + BLE scan for free — both are position sources |
| Uplink | SIM800L | 2G/GSM only. See §"2G availability" |
| GNSS | NEO-6M | Keep V_BCKP powered or every fix is a cold start |
| Trigger | Momentary button | Recessed, RC-debounced, on an RTC-capable GPIO |
| Power | LiPo 2500–3000 mAh + TP4056 | Bigger than the 2000 mAh in PLAN.md — see §"Battery" |
| Bulk cap | 1000–2200 µF electrolytic | **Not optional.** See §"Power warnings" |
| Feedback | **LED** (motor out of stock) | On `PIN_FEEDBACK` — same GPIO the motor will use. See below |
| ~~Motion~~ | ~~LIS3DH~~ | **Not stocked.** Motion is inferred from the Wi-Fi and cell radios instead — see below |

---

## Power warnings — read this first

**The single most common way to destroy a SIM800L build is powering it wrong.**

1. **SIM800L draws up to 2 A in transmit bursts** at 3.4–4.4 V. It **cannot** run from the
   ESP32's 3V3 pin, from an AMS1117, or from most USB regulators. Wire it **directly to the
   LiPo**, which sits happily in that voltage range.
2. **Fit a 1000–2200 µF electrolytic across the SIM800L's VCC/GND, physically close to the
   module.** Without it the TX burst browns out the rail, the ESP32 resets, and you will spend a
   weekend blaming the firmware.
3. **Use thick, short power wires.** Thin jumpers have enough resistance to cause the same
   brownout.
4. **Level-shift the ESP32 TX line.** SIM800L RX is not 3.3 V tolerant — use a divider
   (1 kΩ / 2.2 kΩ) or a proper level shifter. The SIM800L's TX at ~2.8 V reads fine as logic
   HIGH on the ESP32, so that direction needs nothing.
5. **NEO-6M and RC522-class modules are 3.3 V.** 5 V kills them.

If the module resets, reboots the ESP32, or never registers — it is almost always #1 or #2.

---

## Pinout

See [pins.h](include/pins.h) for the authoritative list. Summary:

| Signal | GPIO | Note |
|---|---|---|
| SIM800L TX → ESP32 RX | 16 | UART2 |
| ESP32 TX → SIM800L RX | 17 | **via level shifter** |
| SIM800L PWRKEY | 23 | Pulse low to toggle power |
| SIM800L power gate | 32 | MOSFET, high-side |
| NEO-6M TX → ESP32 RX | 25 | UART1, remapped |
| ESP32 TX → NEO-6M RX | 26 | UART1, remapped |
| NEO-6M power gate | 27 | MOSFET; V_BCKP stays powered |
| SOS button | 33 | RTC-capable → wakes from deep sleep. Active low |
| Child-facing feedback | 13 | LED + series resistor now; motor + MOSFET + flyback later |
| Debug LED | 2 | Onboard — *not* the child-facing one |
| Battery sense | 34 | **ADC1 only** — ADC2 is unusable while Wi-Fi is on |

Two ESP32 traps encoded above: **ADC2 pins cannot be read while Wi-Fi is active**, so battery
sense must live on GPIO 32–39. And **deep-sleep wake needs an RTC GPIO**, which is why the
button is on 33.

---

## Consequence 1 — SIM800L has no usable TLS

SIM800 SSL support is firmware-dependent and unreliable. Do **not** trust it for a child's
location data.

**Do the crypto on the ESP32 instead.** Use the modem as a dumb TCP socket (TinyGSM) and wrap it
in ESP32-side TLS via `SSLClient`, which layers mbedTLS over any `Client`. The ESP32 has the RAM
and the hardware crypto; the modem does not need to know.

The cost is handshake time. On 2G — ~40 kbps with 300–600 ms round trips — a fresh TLS handshake
takes **3–8 seconds**, which blows straight through the five-second SOS deadline.

The tempting fix is to hold a persistent MQTT session open so the handshake is paid once at boot.
**Don't.** Carrier NAT typically drops an idle GPRS TCP session after 1–5 minutes, so holding it
open requires ~60 s keepalives — and those keepalives stop the modem ever reaching `AT+CSCLK`
sleep, the biggest power lever available. You would spend the battery keeping alive a socket the
carrier is going to drop anyway.

**Bursty HTTPS + SMS control is the resolution:** GPRS comes up only when there is something to
send, and SMS carries anything that must be instant or must work while the modem sleeps.

### Why HTTP rather than MQTT

Once GPRS is bursty, MQTT's persistent session buys nothing — and plain HTTP is simpler in three
concrete ways:

- **No broker to run, secure, or operate.** The ingest endpoint is just another FastAPI route, so
  the separate ingest worker disappears from the architecture.
- **The HTTP 200 *is* the server-level ack.** MQTT QoS 1 confirms delivery to the broker, not that
  the server recorded anything — a distinction that is easy to get wrong. With HTTP there is
  nothing to get wrong.
- **It is debuggable with `curl`.** On a 2G link you will want that.

Two rules that come with it: **batch** routine positions so the 3–8 s handshake is amortised over
many points rather than paid per point, and give every event an **id** so a retried POST is
idempotent — a duplicate delivery must never become a duplicate position.

**Set the clock before the first handshake.** TLS validation needs a roughly-correct time and the
ESP32 has no RTC. Pull it from the modem with `AT+CCLK?` after NITZ, or every connection fails
validation for no obvious reason. (The scanner solves the same problem with a DS3231.)

**Do not use the SIM800L's `AT+HTTPSSL`.** Modem-side TLS is the same firmware-dependent path
warned about above. Keep the crypto on the ESP32 via `SSLClient`, with the server certificate
pinned — one endpoint means you can pin rather than ship a CA bundle.

## Consequence 2 — SMS becomes the control channel, both directions

This is the one place the hardware substitution makes the design *better*, and in the Philippines
it is also nearly free.

**Uplink — SOS.** A SIM800L sends an SMS directly to the parent's phone with no GPRS session, no
TLS handshake, and no server in the path. It works when data is congested, when the APN is wrong,
and when your server is down.

```
SOS pressed
  ├─ SMS  → parent's phone directly     ← ~5-10 s, dumb, almost unkillable
  └─ HTTP → server → full escalation    ← ~10-20 s, rich data, ack, audit trail
```

**Downlink — "Locate now."** A parent taps the button on the dashboard; the server sends the device
an SMS; the modem raises RI and wakes even from `CSCLK` sleep; the device takes a fix and reports.
This is what makes on-demand location work *without* holding a TCP socket open against carrier NAT.
Expect 10–20 s end to end.

Two rules for the downlink:
- **Authenticate inbound commands** (`SMS_CMD_SECRET`). An unauthenticated SMS command channel is a
  remote-control interface for anyone who learns the number.
- **Rate-limit them**, so a parent tapping "locate" repeatedly cannot flatten the battery.

Server-side, de-duplicate the SMS against the HTTP event by ID so a parent isn't told twice for one
press.

## No vibration motor — an LED stands in

The motor is out of stock. An LED sits on the same GPIO (`PIN_FEEDBACK`, 13), so fitting the
motor later is a wiring change plus `FEEDBACK_USE_MOTOR` in `config.h` — not a code change.

**This substitution is not neutral, and it is the one place in this project where the stand-in is
genuinely worse than the part.** A buzz is *felt*: it reaches a child whose device is in a pocket
or a backpack, who is frightened, and who is not looking at anything. An LED has to be *looked
at*. Two adaptations follow:

**The ack cue repeats for 20 s** instead of firing once. With haptics, "your parent has been
told" lands the instant it happens. An LED the child is not looking at is a cue that never
arrived — so it keeps pulsing to give them time to look down.

**Nothing blocks.** A 20-second cue cannot sit in `delay()`, so feedback became a millis()-driven
state machine serviced from `loop()`. That also removed the 300 ms `delay()` that used to sit
inside `sos::trigger()` — on the path racing a five-second deadline.

Patterns are shaped so they are distinguishable without counting blinks:

| Cue | Pattern | Means |
|---|---|---|
| Armed | quick flutter, 3× | The 2 s hold registered |
| Cancelled | one long glow | Accidental press aborted |
| Sent | steady 2 s | Transmitted at t+5 s |
| **Acked** | **calm heartbeat, 20 s** | **Your parent has been told** |
| Low battery | single tick | Ambient, lowest priority |

**Mount the LED where the child can actually see it** while wearing the device — a bezel or
light pipe on the front face, not buried in the enclosure.

**If you have a piezo buzzer, use that instead.** Audible cues, like haptic ones, do not require
the child to be looking at the device, so a buzzer is a much closer substitute than an LED. Same
pin, same code.

## No accelerometer — motion comes from the radios

There is no LIS3DH in the BOM, so the cadence state machine is driven by the radios that are
already there. Three tiers, each one's cost matched to its confidence:

| Tier | Cost | Interval | Signal |
|---|---|---|---|
| 0 | free | 60 s | Serving cell + neighbour RSSI. The modem is already awake |
| 1 | ~0.05 mAh | 2–5 min | Wi-Fi scan, Jaccard similarity on the BSSID set. **The authority** |
| 2 | expensive | on demand | GNSS — gated *by* this, never part of it |

**This is arguably the better signal.** An accelerometer answers the wrong question: it reports
that the device is being *jostled*, which a child fidgeting at a desk does all day, and each
false "moving" burns a modem wake. A changed Wi-Fi neighbourhood reports that the child is
somewhere *else*, which is the thing the cadence actually cares about.

**Power cost of the substitution: about 0.5%.** A scan every 5 minutes is ~13 mAh/day against a
2500 mAh cell. The LIS3DH would have drawn ~2 µA. The difference vanishes next to a SIM800L,
which dominates the budget by three orders of magnitude.

**What is genuinely lost** is interrupt-driven wake from deep sleep. That costs little here too:
the modem sits at 1–3 mA in `CSCLK` sleep while the ESP32 deep-sleeps at 20–40 µA, so MCU sleep
depth barely moves the total. The real consequence is that movement is noticed **up to one scan
interval late** — fine for a child tracker, which is not turn-by-turn navigation.

**The failure case to handle:** rural, no APs visible and no cell change. `motion::blind()`
returns true and the caller must fall back to a fixed cadence rather than trusting a
confident-looking "stationary". A tracker that thinks a moving child is sitting still is worse
than one that just reports on a timer.

## Consequence 3 — battery life regresses

SIM800L has no LTE-M power-saving mode. Rough figures:

| State | SIM7080G (planned) | SIM800L (actual) |
|---|---|---|
| Modem idle, attached | 1–3 mA | 10–20 mA, or ~1 mA in `AT+CSCLK=1` sleep |
| TX burst | 200–500 mA | up to 2000 mA |
| Realistic runtime, 2000 mAh | 5+ days | 2–3 days |

Levers, in order of value:
1. **`AT+CSCLK=1` with DTR control** — the modem sleeps between reports and wakes on DTR. This is
   the single biggest win; without it, expect ~2 days.
2. **Power-gate the NEO-6M** (GPIO 27). It draws ~45 mA continuously while tracking. But keep
   **V_BCKP powered** so ephemeris survives — otherwise every fix is a 27 s cold start instead of
   a ~1 s hot one, which costs more energy than the gating saved.
3. **Bigger cell.** 3000 mAh, and plan on charging daily rather than every 3–4 days.

There is a genuine tension here: powering the modem down entirely saves the most, but a cold GSM
attach takes 5–15 s, which delays the SOS. **Keep the modem attached and in `CSCLK` sleep.** For a
safety device, latency wins over runtime.

---

## Philippines — deployment notes

**2G is the right call here.** Globe and Smart both still run extensive 2G, and rural coverage
leans on it heavily. LTE-M and NB-IoT are not meaningfully available to consumers in PH, so the
SIM800L is not a compromise forced by stock levels — it is the correct part for this market.
Carriers have signalled an eventual sunset with no firm date; re-check before a large build.

Still run Phase 0: `AT+CREG?` and `AT+CSQ` along the child's actual route, on **both** Globe and
Smart, since regional coverage differs a lot.

Three local operational details that will bite:

1. **SIM registration is mandatory** (RA 11934). Every prepaid SIM must be registered to a real
   identity or it gets deactivated. Register the tracker's SIM to the parent, and keep the account
   details somewhere you can find them — an unregistered SIM going dark looks exactly like a
   hardware failure.
2. **Prepaid load expires, and the tracker goes silent when it does.** This is a genuine failure
   mode, not an inconvenience. Poll the balance periodically (`*143#` Globe, `*123#` Smart) and
   surface it on the dashboard *before* it hits zero. `BALANCE_CHECK_MS` in config.h.
3. **Typhoon season takes cell sites down.** Nothing to engineer around, but it is the reason the
   flash queue holds events with their original timestamps rather than dropping them.

**Data volume is a non-issue.** A position report is ~250 bytes; even at the 2-minute moving
cadence the device uses roughly **4–6 MB/month**, which the cheapest data promo covers many times
over. TLS handshakes are the expensive part at ~5–6 KB each, which is a second reason to batch
routine reports into bursts rather than connecting per point.

---

## Position sources

The four-source design from PLAN.md survives the hardware change intact, because the ESP32
provides two of them itself:

| # | Source | Hardware | Where it wins |
|---|---|---|---|
| 1 | GNSS | NEO-6M | Outdoors |
| 2 | Wi-Fi AP scan | ESP32 native | **Indoors — the primary** |
| 3 | BLE anchors | ESP32 native | Known places: home, school |
| 4 | Cell tower ID | SIM800L `AT+CENG` | The floor |

Wi-Fi scanning is what makes this work inside a school, and it costs no extra hardware.

---

## Build

```bash
pio run -t upload -e tracker && pio device monitor
```

Copy `include/config.h` values into place first — APN, broker host, parent numbers, and the
device credential are all there.
