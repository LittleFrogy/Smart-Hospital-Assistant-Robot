# Troubleshooting

## 1. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| ESP32-CAM appears dead, reboots constantly | Missing or too-small bulk capacitor | Fit 470µF within 20mm of the module |
| ESP32-CAM never runs your code after flashing | GPIO0 still jumpered to GND | Remove the jumper, press RST |
| Cannot upload to ESP32-CAM | Adapter set to 3.3V, or RST not pressed | Set adapter to 5V, press RST as upload begins |
| LCD blank or shows solid blocks | Contrast, or wrong I2C address | Turn the backpack trimpot; run an I2C scanner |
| LCD garbage characters | Missing level shifter, 5V on SDA/SCL | Fit the BSS138 or mod the pull-ups to 3.3V |
| Motors very weak or won't start | Buck #1 set too low, or 5V used for the motor rail | Set Buck #1 to 7.5V |
| Motors run hot, smell | Buck #1 set too high, or battery wired direct | Never exceed 7.5V into `+12V` |
| ESP32 resets when motors start | Missing 1000µF on the motor rail | Fit it at the L298N terminals |
| ESP32-CAM reboots when the lock actuates | Servo commanded while motors running | Enforce the §5.2 firmware constraint |
| IR array reads all the same regardless of surface | Wrong mounting height, or powered at 3.3V with poor sensitivity | Adjust height 5–20mm; if still poor, power at 5V **with** dividers on all five outputs |
| Navigation logic inverted, stops on white | Polarity assumption backwards | Black = HIGH per your datasheet — verify with the print sketch |
| Car oscillates side to side on the line | P gain too high for the front-caster geometry | Reduce P, add a little D |
| Distance readings erratic or always zero | ECHO divider wrong, or HC-SR04 on 3.3V | Measure the midpoint; power the sensor at 5V |
| Car stops repeatedly at the turn zone | Missing marker cooldown after the spin | Add the 2000ms cooldown (phase 5) |
| Spin overshoots and never finds the line | Phase 2 or 3 PWM too high | Phase 2 at 55%, phase 3 at 35% |
| Spin drags or stutters | Swivel caster fighting the rotation | Replace with a ball caster |
| Car slips, spin timing inconsistent | Weight too far forward | Move the pack over or behind the drive axle |
| Web page unresponsive for 30s after delivery | `delay()` used for the auto-lock | Use `millis()` |
| Boards unreachable after a router reboot | DHCP reassigned addresses | Set static IPs with `WiFi.config()` |
| Buzzer never sounds | S8050 pinout reversed | Check E–B–C vs C–B–E on your part |

---

## 2. Warning summary

| # | Warning |
|---|---|
| W1 | TT motors are 3–6V. Never feed the L298N 11.1V direct — use the 7.5V buck. |
| W2 | ESP32 GPIO absolute maximum is 3.6V. HC-SR04 ECHO needs the divider; the IR array must run at 3.3V. |
| W3 | The I2C LCD backpack pulls SDA/SCL to 5V. Use the level shifter or mod the pull-ups. |
| W4 | Never actuate the lock servo while the motors run. |
| W5 | ESP32-CAM has no USB port. You need the CP2102 adapter to flash it. |
| W6 | ESP32-CAM `GPIO16` is PSRAM chip select — leave it unconnected. |
| W7 | The ESP32-CAM 470µF cap is mandatory. Without it the module reboot-loops and looks dead. |
| W8 | 3S LiPo needs a balance charger. Fit the fuse and the low-voltage alarm. |
| W9 | Remove the ESP32-CAM's GPIO0 jumper after flashing or the board never runs your code. |
| W10 | Use `millis()`, never `delay()`, for the 30-second timer. |
| W11 | Use a ball caster, not a swivel caster — a swivel caster fights the 180° spin. |
| W12 | Never connect a motor terminal to ground, to the battery, or to any ESP32 pin. Motors go only to the H-bridge outputs. |
