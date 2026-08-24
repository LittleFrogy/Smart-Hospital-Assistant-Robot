# Dispenser firmware

Arduino sketch for the ESP32 DevKit V1 on the stationary unit. The simplest board in the project.

## Libraries

| Library | Why |
|---|---|
| `WiFi.h` | Built in |
| `ESPAsyncWebServer` + `AsyncTCP` | REST endpoints |
| `ESP32Servo` | **Required** — the stock `Servo` library does not work on ESP32 |

## Endpoints

| Endpoint | Method | Action |
|---|---|---|
| `/drop` | POST | Rotate SG90 open, hold 800ms, close |
| `/status` | GET | `{"state":"ready"}` or `{"state":"busy"}` |

## Behaviour

- **Heartbeat:** blink GPIO2 (onboard blue LED) once per second when Wi-Fi is connected, fast while reconnecting, solid during a drop cycle. Free diagnostics — no wiring.
- **Optional button:** GPIO4 with `INPUT_PULLUP`, active LOW, 50ms software debounce. Triggers the same drop routine as `/drop`.
- **Busy guard:** reject a second `/drop` while a cycle is in progress.

Use a static IP with `WiFi.config()`. DHCP will reassign after a router reboot.

Wiring: [06 — Dispenser wiring](../../docs/06-dispenser-wiring.md).
