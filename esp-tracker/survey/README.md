# Survey — Phase 0 go/no-go

Answers the two questions that decide whether the rest of this project is buildable, before you
solder anything permanent. Both are a weekend. Both are ruinous to discover in Phase 3.

1. **Does 2G actually register with usable signal along the child's real route?**
2. **Are there enough Wi-Fi APs indoors — and does the geolocation provider know where they
   are?** These are different questions, and only the second one matters.

## Hardware

The survey rig *is* the tracker prototype — same pin map, so nothing is wasted:
ESP32 + SIM800L + NEO-6M + button, on a USB power bank.

The SIM800L power rules from [../tracker/README.md](../tracker/README.md) apply here too: direct
to the LiPo or a supply that can deliver **2 A bursts**, with a **1000–2200 µF cap close to the
module**, and a level shifter on the ESP32 TX line. A survey that browns out mid-route just
tells you your wiring is wrong.

## Use

```bash
pio run -t upload -e survey
```

Then unplug from the laptop, put it on a power bank, and walk the route.

| Action | Effect |
|---|---|
| Normal boot | Logging starts. One sample per 10 s. |
| **Short press** | Marks a waypoint — press at the gate, in the classroom, at home |
| **Hold 3 s** | Erases the log (5 fast blinks first) |
| **Boot with button held** | Dumps `survey.csv` to serial |

Blink code after each sample, so you can read it without a screen:

| Blinks | Meaning |
|---|---|
| 3 | Registered, decent signal |
| 2 | Registered but weak (CSQ < 10) |
| **1** | **Not registered** — this is the finding that matters |

Waypoints are the point of the exercise. A route average hides the classroom where nothing
works; that one room is what you actually need to know about.

## Analysis

Reboot with the button held, save the dumped block as `survey.csv`, then:

```bash
python3 analyze.py survey.csv
```

Add a provider key to close gate 2 properly:

```bash
python3 analyze.py survey.csv --geolocate YOUR_UNWIREDLABS_KEY
```

**This second step is not optional.** Counting access points only proves they exist. It does not
prove the provider's database knows where they are — and in a newly built subdivision it very
often does not. An AP the provider has never seen is worth nothing to you.

## The gates

| Gate | Threshold |
|---|---|
| **1 — uplink** | Registered ≥ 98% of samples, CSQ median ≥ 15 along the route |
| **2 — indoor fix** | ≥ 5 APs *and* provider resolves to ≤ 50 m **inside the school** |

**Run it once per carrier.** Globe and Smart coverage differs a lot by area, and the answer
decides which SIM the device carries for its whole life.

## If a gate fails

**Gate 1 fails on both carriers** — the SIM800L cannot serve that route. LTE-M is not a
meaningful option in PH, so the honest answers are a different route, an external antenna, or
accepting that the device is silent on that stretch and the flash queue delivers late.

**Gate 2 fails inside the school** — Wi-Fi positioning will not work there, and GNSS already
does not. That is exactly what the BLE anchors are for: a ~$3 beacon by the classroom door gives
certainty where the estimate was going to fail anyway. Budget for two or three.
