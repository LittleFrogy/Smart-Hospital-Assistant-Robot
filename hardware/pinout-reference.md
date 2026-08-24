# Pinout Reference

One-page GPIO map for both boards. Full wiring detail in [05 — Car wiring](../docs/05-car-wiring.md) and [06 — Dispenser wiring](../docs/06-dispenser-wiring.md).

## Board

**DOIT ESP32 DevKit V1, 30-pin**, CP2102 USB-serial, USB-C connector.

Left header, top to bottom:
`EN, VP, VN, D34, D35, D32, D33, D25, D26, D27, D14, D12, D13, GND, VIN`

Right header, top to bottom:
`D23, D22, TX0, RX0, D21, D19, D18, D5, TX2, RX2, D4, D2, D15, GND, 3V3`

## Car controller

| GPIO | Pin label | Direction | Function |
|---|---|---|---|
| 4 | `D4` | In | HC-SR04 ECHO (via 1k/2k divider) |
| 13 | `D13` | Out | HC-SR04 TRIG |
| 14 | `D14` | Out PWM | L298N ENA |
| 16 | `D16` | Out | Status LED (via 220R) |
| 17 | `TX2` | In | IR array OUT5 |
| 18 | `D18` | Out PWM | Lock servo (via BSS138 LV3) |
| 19 | `D19` | Out | Buzzer (via 1k to S8050 base) |
| 21 | `D21` | Bidir | I2C SDA (via BSS138 LV1) |
| 22 | `D22` | Out | I2C SCL (via BSS138 LV2) |
| 23 | `D23` | Out PWM | L298N ENB |
| 25 | `D25` | Out | L298N IN3 |
| 26 | `D26` | Out | L298N IN2 |
| 27 | `D27` | Out | L298N IN1 |
| 32 | `D32` | Out | L298N IN4 |
| 33 | `D33` | — | **Spare** (ADC1 capable) |
| 34 | `D34` | In only | IR array OUT3 (centre) |
| 35 | `D35` | In only | IR array OUT4 |
| 36 | `VP` | In only | IR array OUT1 (leftmost) |
| 39 | `VN` | In only | IR array OUT2 |

Power: `VIN` ← 5V rail, `GND` → star ground, `3V3` → IR array VCC.

## Dispenser controller

| GPIO | Pin label | Direction | Function |
|---|---|---|---|
| 2 | — | Out | Onboard blue LED (heartbeat, no wiring) |
| 4 | `D4` | In | *Optional* push button → GND, `INPUT_PULLUP` |
| 18 | `D18` | Out PWM | Drop gate servo |

Power: `VIN` ← 5V rail, `GND` → star ground.

## ESP32-CAM (AI-Thinker)

Runtime: `5V` and `GND` only. Everything else unconnected.

| Pin | Status |
|---|---|
| `5V`, `GND` | Connect to the 5V rail and star ground |
| `GPIO16` | **Never connect** — PSRAM chip select |
| `GPIO0` | Ground only while flashing, then remove |
| `U0T`, `U0R` | Flashing only, to the CP2102 adapter |
| All others | Camera bus or SD card — leave open |

## Pins never to use, either board

| Pin | Reason |
|---|---|
| GPIO 0, 2, 5, 12, 15 | Strapping pins — affect boot mode and flash voltage |
| GPIO 1, 3 | TX0/RX0, USB serial |
| GPIO 6–11 | SPI flash die (not broken out on the 30-pin board) |

This design uses **zero strapping pins**, so both boards boot reliably regardless of peripheral state at power-on.

## ADC note

ADC2 pins (0, 2, 4, 12–15, 25–27) are unusable for analog reads while Wi-Fi is active. Digital I/O on those pins is unaffected. GPIO33 is left spare specifically because it is ADC1-capable — use it for a battery monitor if you want one.
