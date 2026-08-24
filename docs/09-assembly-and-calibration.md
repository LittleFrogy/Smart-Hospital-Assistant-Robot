# Assembly and Calibration

## 1. Assembly and bring-up order

Do not skip ahead — each step depends on the previous one working.

| # | Action | Verify before continuing |
|---|---|---|
| 1 | Charge the pack on a balance charger, fit the LiPo alarm | Each cell 4.15–4.20V |
| 2 | Assemble chassis: motors rear, ball caster front, shim level | Chassis sits flat, caster spins freely |
| 3 | Wire fuse, switch, both bucks — **no loads** | 7.5V and 5.00V on the meter |
| 4 | Build the star ground block | Continuity everywhere, <1Ω |
| 5 | Power the car ESP32 alone | Boots, serial output |
| 6 | Add ESP32-CAM + its 470µF, flash a test sketch | Camera stream reachable over Wi-Fi |
| 7 | Add shifter + LCD, run I2C scanner | `0x27` or `0x3F` found, text displays |
| 8 | Add lock servo, sweep test | Moves cleanly, no ESP32 reset |
| 9 | Add buzzer + LED | Respond, no rail sag |
| 10 | **Add the IR array, run a 5-channel print sketch** | Confirm polarity, set mounting height |
| 11 | Add HC-SR04 — **measure the divider midpoint first** | ≤3.4V, stable distance readings |
| 12 | Wire L298N, **motors disconnected** | Measure OUT1–OUT4 while toggling |
| 13 | Connect motors, wheels **off the ground** | Correct direction each side |
| 14 | Line following on a straight line | Tracks without oscillating |
| 15 | Marker stop detection | Halts on every strip, debounced |
| 16 | Sonar obstacle stop | Halts at 25cm, resumes when clear |
| 17 | Build the dispenser separately | Drop cycle works standalone |
| 18 | **Calibrate the spin** (§12.1) | 180° in place, re-acquires line, 5 of 5 |
| 19 | **Full cycle with manual nurse buttons** | Complete delivery, start to finish |
| 20 | Python QR decode + `/resume` dispatch | Skips wrong beds automatically |
| 21 | Missing-QR test — cover the target's code | Reaches turn zone, spins, returns **locked** |

**Steps 10 and 11 are the ones people skip.** Step 10 saves you a day of debugging inverted logic. Step 11 takes ten seconds with a multimeter and prevents an unrecoverable pin failure.

**Milestone 19 is the real deadline.** Everything after it is polish. A robot that completes the full cycle with a nurse clicking confirm is a far better demo than a half-finished autonomous one.

---

## 2. Calibration reference

| What | How |
|---|---|
| Buck #1 | 7.5V, no load, multi-turn trimmer |
| Buck #2 | 5.00V, no load |
| Caster height | Shim until the chassis sits level with the drive wheels |
| Motor direction | Wheels off ground, command forward; if a wheel reverses, **swap that motor's two wires at the OUT terminals** — fix it physically, not in software |
| IR array height | Start 10mm, adjust 5–20mm for clean transitions. No trimpots — height is the only knob. |
| IR polarity | Print all five channels, slide over line and floor. Expect black = HIGH. |
| Line-following P gain | Start low. Front-caster geometry means high sensor gain — reduce P until oscillation stops, then add D. |
| Spin blind time | Average five timed 180° spins at 55% PWM, use 80% of the average |
| Station stop offset | Measure array-to-compartment distance; creep that far past the marker before stopping |
| Sonar threshold | 25–30cm. Treat readings below 4cm as invalid, not "very close". |
| LCD contrast | Blue trimpot on the backpack, turn until characters appear |
