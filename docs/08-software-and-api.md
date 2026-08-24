# Software and API

## 1. Web API

### 1.1 Architecture

You need Python on the PC for QR decoding anyway. Use it to host one page controlling both units, rather than two ESP32 web servers in two browser tabs.

### 1.2 Dispenser endpoints

| Endpoint | Method | Action |
|---|---|---|
| `/drop` | POST | Rotate SG90 open, hold 800ms, close |
| `/status` | GET | `{"state":"ready"}` or `{"state":"busy"}` |

### 1.3 Car endpoints

| Endpoint | Method | Action |
|---|---|---|
| `/go?bed=N` | POST | Set target, begin outbound run |
| `/lock` | POST | Close lock servo |
| `/unlock` | POST | Open lock, start 30s auto-lock timer |
| `/resume` | POST | Wrong bed — continue to the next marker |
| `/status` | GET | State, last scanned ID, distance, sensors |
| `/abort` | POST | Stop motors immediately, return to station |

`/resume` is what makes QR-only navigation work: the PC decodes, sees the wrong bed, and tells the car to keep going. Without it the car sits at the first marker forever.

Add `/abort` even if the demo never uses it. When something goes wrong in testing you want a stop button that isn't chasing the car across the floor.

### 1.4 ESP32-CAM

- `http://<cam-ip>/capture` — single JPEG, what Python pulls
- `http://<cam-ip>:81/stream` — MJPEG live view, embed in the Flask page

---

## 2. QR verification

### 2.1 Primary — PC decodes

```python
import requests, io, time
from pyzbar.pyzbar import decode
from PIL import Image

def read_marker(cam_ip, attempts=3):
    for _ in range(attempts):
        img = requests.get(f"http://{cam_ip}/capture", timeout=5).content
        codes = decode(Image.open(io.BytesIO(img)))
        if codes:
            return codes[0].data.decode()
        time.sleep(1.0)
    return None
```

Three attempts with a 1-second gap — a single frame can miss due to focus or glare; three rarely all fail. On the return leg use **one** attempt: the car only cares about finding `STATION`, so a fast miss is what you want.

### 2.2 Fallback — nurse manual verify

When `read_marker` returns `None`, the Flask page switches to manual mode: live MJPEG stream, the expected bed number, and two buttons — **"Confirm and unlock"** and **"Wrong bed — continue."**

This is not a workaround. Real clinical systems have manual overrides, and having one is a design strength to point at during your presentation.

### 2.3 Development order

Even though automatic decoding is the intended runtime path, **wire up the manual buttons first and get the full cycle running with them.** Debug navigation, servos, and timing while verification is trivially reliable. Then add the Python decoder on top. If it misbehaves, you fall back without touching any other code.

---

## 3. State machine and timings

| State | Entry action | Exit condition |
|---|---|---|
| `STANDBY` | LCD "Ready for Next Delivery", lock **open** | `/go?bed=N` received |
| `AT_STATION` | Buzzer chirp 200ms, LCD "Awaiting medicine" | Drop confirmed |
| `LOCKING` | Servo to locked | 600ms |
| `DRIVING` | Line-follow, sonar active | All five IR black for 60ms |
| `SCANNING` | Stop motors, hold 1000ms for camera settle, LCD "Verifying..." | PC returns an ID, or timeout |
| `DELIVERING` | Servo unlock, LED on, buzzer, start 30s timer | 30s elapsed |
| `AUTOLOCK` | Servo to locked, LCD "Locked" | 600ms |
| `SPINNING` | Run the 5-phase spin (§12), set `returning = true` | Cooldown expired |
| `FAULT` | Stop, LED pattern, buzzer, LCD error | Manual reset or `/abort` |

There is no `OUTBOUND`/`RETURN` distinction in the motion states — the car drives the same way in both directions. Only a `returning` boolean changes how `STATION` is interpreted.

### 3.1 Timings

| Event | Duration | Reason |
|---|---|---|
| Camera settle after stopping | **1000ms** | Chassis rocks after braking; motion blur kills decoding |
| QR attempts, outbound | **3 × 1s** | Then hand off to nurse |
| QR attempts, returning | **1 × 1s** | Only looking for `STATION` |
| Servo travel | **600ms** | SG90 needs ~500ms for 90°, plus margin |
| Auto-lock after unlock | **30000ms** | Spec |
| Marker debounce on / off | **60ms / 300ms** | Reject noise, prevent double-counting |
| Spin blind phase | **~880ms** | Calibrate per §12.1 |
| Marker cooldown after spin | **2000ms** | Prevents re-triggering the turn-zone marker |
| Obstacle stop distance | **25cm** | Below 20cm the car cannot stop in time |
| Nurse verify timeout | **120000ms** | Then fault and return locked |

### 3.2 The 30-second auto-lock

Use `millis()`, **never** `delay()`. During those 30 seconds the car must still serve `/status`, run the LCD countdown, and respond to `/abort`.

```cpp
if (state == DELIVERING && millis() - unlockTime >= 30000) {
    lockServo.write(LOCK_ANGLE);
    state = AUTOLOCK;
}
```

Chirp the buzzer at 10s, 5s, and 3s remaining so the patient knows the compartment is about to close.

A `delay(30000)` would freeze the web server and make the car unresponsive for half a minute.

---

## 4. Failure modes

| Failure | Detection | Response |
|---|---|---|
| Medicine didn't drop | No confirmation 3s after `/drop` | Buzzer, LCD "Dispenser jam", stay at station |
| Wrong bed reached | QR ID ≠ target | **Keep locked**, `/resume` to next marker |
| Target QR missing/damaged | Reached `TURNAROUND` still holding medicine | Spin, return, LCD "Target not found", **stay locked** |
| QR unreadable | 3 decode attempts fail | Nurse manual verify via live stream |
| Nurse doesn't respond | 120s in `SCANNING` | Fault, return to station **locked** |
| Spin failed to re-acquire line | Phase 3 exceeds 3000ms | Stop, fault — do not drive blind |
| Line lost | All five read white >1.5s | Stop, reverse 3cm, re-acquire; fault after 2 tries |
| Obstacle persists | Sonar <25cm for >20s | Buzzer, hold position, flag in `/status` |

**Demo the wrong-bed case deliberately.** A robot that moves is a project; a robot that refuses to deliver to the wrong patient is a system.
