# Car firmware

Arduino sketch for the ESP32 DevKit V1 on the mobile unit.

## Libraries

| Library | Why |
|---|---|
| `WiFi.h` | Built in |
| `ESPAsyncWebServer` + `AsyncTCP` | REST endpoints |
| `ESP32Servo` | **Required** — the stock Arduino `Servo` library does not work on ESP32 |
| `LiquidCrystal_I2C` | 16×2 display over the PCF8574 backpack |

## Board settings

- Board: `ESP32 Dev Module`
- Upload speed: `921600`
- Port: whichever COM port the CP2102 enumerates as

## Pin constants

See [`hardware/pinout-reference.md`](../../hardware/pinout-reference.md). Define them all at the top of the sketch so nothing is hard-coded inline.

## Structure

```
setup()
  ├─ pinMode all inputs and outputs
  ├─ ledcSetup for ENA, ENB, servo
  ├─ lcd.init()
  ├─ WiFi.config() with a static IP, then WiFi.begin()
  └─ register endpoints, server.begin()

loop()
  ├─ readSensors()      five IR channels, sonar distance
  ├─ updateStateMachine()
  └─ updateOutputs()    motors, servo, LCD, buzzer, LED
```

## Non-negotiable rules

1. **`millis()`, never `delay()`.** A `delay(30000)` for the auto-lock freezes the web server for half a minute.
2. **Never command the servo while the motors run.** Only `LOCKING`, `DELIVERING`, and `AUTOLOCK` touch the servo, and all three zero the motors on entry. Ignoring this browns out the ESP32-CAM.
3. **Verify IR polarity before writing navigation logic.** Per the datasheet, black = HIGH. Confirm with a print sketch first.

## First sketch to write

Not the state machine — a five-channel sensor print:

```cpp
void loop() {
  Serial.printf("%d %d %d %d %d\n",
    digitalRead(36), digitalRead(39), digitalRead(34),
    digitalRead(35), digitalRead(17));
  delay(200);
}
```

Slide the array over line and floor. Confirm polarity and set the mounting height before anything else.

State machine, endpoints, and timings: [08 — Software and API](../../docs/08-software-and-api.md).
