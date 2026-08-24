# Overview and Features

## 1. Confirmed configuration

| Item | Value |
|---|---|
| Architecture | Two independent units, no wires between them, coordinated over Wi-Fi via PC |
| Car boards | 1 × ESP32 DevKit V1 (30-pin, CP2102, USB-C) + 1 × ESP32-CAM (AI-Thinker) |
| Dispenser board | 1 × ESP32 DevKit V1 (30-pin) |
| Servos | 2 × SG90 — one car lock, one dispenser drop gate |
| Line sensor | 1 × 5-channel IR array, **digital** output, 3.3–5V, no trimpots |
| Drive | 2 × TT gear motors (rear), 1 × ball caster (front) |
| Batteries | 3S LiPo (11.1V nominal, 12.6V full, 9.9V cutoff) |
| Stop signal | All five IR channels read black — the only stop condition |
| Location identity | **QR code only.** No marker counting, no dead reckoning. |
| Drop trigger | Website click |
| Turnaround | 180° spin in place at a dedicated turn zone |
| QR verification | PC decodes (primary), nurse manual verify (fallback) |
| Auto-lock | 30 seconds after unlock |

---

## 2. Full feature list

### Car — autonomous behaviour

1. **Line following** — weighted position from all five IR channels, PID-corrected differential drive
2. **Marker detection** — all five channels black triggers a full stop
3. **Obstacle detection** — HC-SR04 halts the car at 25cm, resumes automatically when clear
4. **QR capture** — ESP32-CAM photographs the code at each marker and serves it over Wi-Fi
5. **Location dispatch** — acts on the decoded string: deliver, resume, spin, or park
6. **Secure compartment** — SG90 lock, closed for the entire journey
7. **30-second auto-lock** — closes automatically after collection, with buzzer warnings at 10s, 5s, 3s
8. **180° spin turnaround** — five-phase sequence with line re-acquisition
9. **Wrong-bed rejection** — lock stays closed, car moves on to the next marker
10. **Missing-QR safe return** — reaches the turn zone, spins, returns still locked
11. **Return to standby** — parks at the station, LCD shows readiness
12. **LCD status** — destination, verification state, countdown, errors
13. **Audible alerts** — arrival chirp, delivery confirmation, error tone
14. **Wi-Fi REST API** — remote command and telemetry

### Dispenser — station behaviour

15. **Website-triggered drop** — SG90 gate opens on command, closes automatically
16. **Wi-Fi REST API** — `/drop` and `/status`
17. **Heartbeat indicator** — onboard GPIO2 LED shows connection state
18. **Optional manual button** — dispenses if Wi-Fi is down

### PC / web layer

19. **Single control page** — commands both units from one browser tab
20. **Bed selection** — choose the destination before dispatch
21. **Live camera stream** — MJPEG from the ESP32-CAM
22. **Automatic QR decode** — `pyzbar` on captured frames
23. **Nurse manual verify** — confirm/reject buttons when automatic decode fails
24. **Abort** — immediate stop and return, from the browser

---

## 3. System architecture

![Delivery cycle](diagrams/delivery-cycle.svg)


```
Browser ──HTTP──> Flask on PC ──HTTP──> Car ESP32         (static IP)
                               ├──HTTP──> Dispenser ESP32  (static IP)
                               └──HTTP──> ESP32-CAM        (static IP)
```

No wires run between the two units.

Give all three boards **static IPs** with `WiFi.config()`. DHCP will reassign addresses after a router reboot and your Flask app will lose them mid-demo.

Campus and hospital Wi-Fi commonly blocks device-to-device traffic. Use a dedicated router or a phone hotspot.

### 3.1 Operating sequence

| Step | Actor | Action |
|---|---|---|
| 1 | PC | Nurse selects bed number, clicks dispense |
| 2 | PC | Sends `/go?bed=N` to the car, `/drop` to the dispenser |
| 3 | Dispenser | SG90 rotates the gate; medicine falls into the car's compartment |
| 4 | Car | Lock servo closes. LCD shows the destination. |
| 5 | Car | Line-follows forward until all five IR channels read black. Stops. |
| 6 | Car | ESP32-CAM captures the QR; PC decodes with `pyzbar` |
| 7 | PC | Compares the decoded ID to the target |
| 8a | PC | Match → `/unlock`. Lock opens, LED on, buzzer sounds. |
| 8b | PC | No match → `/resume`. Car continues to the next marker. |
| 8c | PC | Unreadable → nurse verifies via live stream and confirms manually |
| 9 | Car | 30 seconds after unlock, compartment auto-locks |
| 10 | Car | Continues to the turn zone, spins 180°, returns |
| 11 | Car | Stops at `STATION`, enters standby |
