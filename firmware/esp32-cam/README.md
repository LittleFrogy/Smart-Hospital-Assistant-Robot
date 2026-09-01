# ESP32-CAM — setup, flashing and testing

Complete walkthrough for getting the camera module working and viewing the live
feed on your phone. Follow it in order; nothing here can be skipped.

Sketch: [`esp32-cam.ino`](esp32-cam.ino)

**What you end up with:** the camera joins your Wi-Fi and serves its own web
page. Any phone on the same network opens `http://robotcam.local/` and sees the
live feed. No PC, no app, no cloud.

---

## 0. What you need

| Item | Notes |
|---|---|
| AI-Thinker ESP32-CAM | The one with the OV2640 on a ribbon cable |
| CP2102 USB-TTL adapter | Use its **5V** pin. Leave `3V3` unconnected |
| 4 jumper wires | Camera to adapter. Female-to-female if both have pins |
| 1 more jumper wire | For the `IO0 → GND` link, camera to camera |
| 470µF electrolytic capacitor | 6.3V or higher. Not needed for bench testing — see §7.1. **Required** before the camera goes on the car |
| A 2.4 GHz Wi-Fi network | Your home router is fine. The ESP32 cannot use 5 GHz |
| A phone to view the feed | Any device on the same network |

---

## 1. Install the Arduino IDE and the ESP32 core

1. Install **Arduino IDE 2.x** from arduino.cc.
2. `File → Preferences → Additional Boards Manager URLs`, paste:

   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```

3. `Tools → Board → Boards Manager`, search **esp32**, install
   **esp32 by Espressif Systems**. It is a ~1 GB download and takes a while.

> The red squiggle under `#include <ESPmDNS.h>` in VS Code disappears once this
> is installed. VS Code does not compile the sketch — Arduino IDE does.

No extra libraries are needed. `esp_camera`, `WiFi` and `ESPmDNS` all ship with
the core.

---

## 2. Board settings

`Tools →` set every one of these:

| Setting | Value |
|---|---|
| Board | **AI Thinker ESP32-CAM** |
| Upload Speed | **115200** |
| Flash Frequency | 80MHz |
| Flash Mode | QIO |
| Partition Scheme | **Huge APP (3MB No OTA / 1MB SPIFFS)** |
| PSRAM | Enabled |
| Port | whichever COM port the CP2102 shows up as |

**Partition scheme matters.** The camera library is large; on the default
partition the build fails with *"Sketch too big"*.

If no COM port appears, install the **CP210x VCP driver** from Silicon Labs,
unplug and replug the adapter, then re-check `Tools → Port`.

---

## 3. Wire the CP2102 for flashing

There are **two separate things** to wire: four wires between the boards, and
one link on the camera itself.

### 3.1 Four wires, camera to adapter

| ESP32-CAM | CP2102 |
|---|---|
| `5V` | `5V` |
| `GND` | `GND` |
| `U0T` | `RXD` |
| `U0R` | `TXD` |

`U0T→RXD` and `U0R→TXD` are crossed on purpose. TX talks to RX.

**The CP2102's `3V3` pin stays unconnected.** You power the camera from `5V`
only. If your adapter has a 3.3V/5V *jumper* instead of separate pins, set it to
5V. Either way, never feed anything into the camera's `3V3` pin while `5V` is
connected.

### 3.2 One link, camera to camera

| From | To |
|---|---|
| `IO0` on the ESP32-CAM | `GND` on the ESP32-CAM |

This one does **not** touch the CP2102. It is a jumper between two pins on the
camera board, and it is what puts the ESP32 into bootloader mode.

On the AI-Thinker board `IO0` and `GND` sit next to each other on the same
header, so a single female-female jumper wire bridges them directly.

**Remove this link after flashing.**

> The board has more than one `GND` pin. They are all the same net, connected
> together on the PCB, so it does not matter which you use. Take the one beside
> `IO0` for the bootloader link and any other one for the adapter's ground.

---

## 4. Change the two lines that need changing

Open `esp32-cam.ino`. In **section 1** set your Wi-Fi name and password:

```cpp
const char* WIFI_SSID = "YourWiFiName";
const char* WIFI_PASS = "YourWiFiPassword";
```

That is the only edit required. The addresses below it are already set for a
home router on `192.168.0.x`.

The network must be **2.4 GHz**. The ESP32 cannot see a 5 GHz network at all —
not as a weak signal, it simply does not exist to the radio. Most home routers
broadcast both bands under one name, which works fine.

### Addressing

Each board gets a fixed last octet. That is the only thing identifying it:

| Device | Last octet | Set in |
|---|---|---|
| Router (gateway) | `.1` | — |
| Car ESP32 | `.51` | `car.ino` |
| Dispenser ESP32 | `.52` | `dispenser.ino` |
| **ESP32-CAM** | **`.53`** | `esp32-cam.ino` |

All four files — those three plus `pc/app.py` — are set to `192.168.0.x` to
match your router. Confirm that is right for your network:

```
ipconfig
```

Under `Wireless LAN adapter Wi-Fi`, check `Default Gateway`. If it says
`192.168.0.1` you are done. If it says something else, change the third octet
in all four files to match.

### Moving to another network later

Only the third octet changes:

| Network | Subnet | `USE_STATIC_IP` |
|---|---|---|
| Home router | whatever `ipconfig` shows | `true` |
| Windows laptop hotspot | always `192.168.137.x`, laptop at `.1` | `true` |
| Phone hotspot | varies by phone, sometimes randomised | **`false`** |

Phone hotspots pick their own subnet — Android is usually `192.168.43.x` but
newer versions randomise it, and iPhone uses `172.20.10.x` on a /28 with only
14 usable addresses, where `.51` does not exist. On DHCP the board still
answers to `robotcam.local`.

If a static address fails, the sketch retries on DHCP by itself and says so on
serial. A wrong subnet is never a silent dead board.

> **For a laptop hotspot**, `Settings → Network & internet → Mobile hotspot`.
> Set **Band to 2.4 GHz** — Windows 11 defaults to "Any available", which can
> land on 5 GHz. Turn off the "automatically turn off" power-saving option, and
> note that Windows usually wants an active connection to share.

---

## 5. Flash it

Order matters here.

1. `IO0 → GND` jumper **in place**.
2. Plug the CP2102 into the PC.
3. Press the `RST` button on the ESP32-CAM once.
4. Click **Upload** in Arduino IDE.
5. When the console prints `Connecting.....`, press `RST` once more.
6. Wait for `Hard resetting via RTS pin...`.
7. **Remove the `IO0 → GND` jumper.**
8. Press `RST` again to run the sketch.

> **If you leave `IO0` grounded the board stays in bootloader mode forever and
> looks completely dead.** This catches almost everyone once. If nothing happens
> on boot, check this first.

---

## 6. First boot — the serial test

`Tools → Serial Monitor`, set the baud rate to **115200**, press `RST`.

You should see:

```
=== Smart Hospital Assistant Robot -- ESP32-CAM ===
PSRAM: found
Camera OK at 400x296
  LOW_POWER_BENCH is ON (no capacitor fitted).
  Reduced frame, clock and Wi-Fi power. Turn it off in
  section 2 once you have the 470uF cap, for full QR range.
  connecting to "YourWiFi" (static)....
mDNS up: http://robotcam.local/

-----------------------------------------------
  Live view :  http://192.168.0.53/
               http://robotcam.local/
  Snapshot  :  http://192.168.0.53/capture
  Stream    :  http://192.168.0.53:81/stream
  Status    :  http://192.168.0.53/status
  Signal    :  -47 dBm
-----------------------------------------------
```

If it says `(DHCP)` and reports a different address, the static config was
rejected and it fell back — check your network really is on `192.168.0.x`
with `ipconfig`.

The white LED also **flashes twice** when the servers start. That is your
"it booted and it is serving" signal — useful once the board is on the car and
you cannot see the serial monitor.

**Write down the IP address it printed.** You need it in §9.

Check these three lines specifically:

- `PSRAM: found` — if it says NOT FOUND, the sketch drops to 320×240 and QR
  range gets very short. Usually a fake/clone board.
- `Camera OK at ...` — if it says FAILED instead, go to §10. The resolution
  shown is 400×296 while `LOW_POWER_BENCH` is on, 640×480 once you turn it off.
- `Signal: -47 dBm` — anything better than −70 dBm is fine. Worse than −75 and
  the stream will stutter; move the camera closer to the router.

---

## 7. Runtime wiring — after flashing

Disconnect everything, then rewire for normal operation:

| ESP32-CAM | To |
|---|---|
| `5V` | 5V supply (the 5.0V buck rail on the car, or a 5V 2A USB adapter on the bench) |
| `GND` | Supply ground |
| everything else | **Leave unconnected** |

**Solder a 470µF capacitor across the module's `5V` and `GND`, within 20 mm of
the board.** The ESP32 draws ~300 mA spikes when the Wi-Fi radio transmits. On
a long thin jumper wire that spike collapses the rail and the board resets. The
symptom is a board that boots, prints an IP, then reboot-loops the moment you
open the stream — and it looks exactly like a dead board.

If the serial monitor ever shows:

```
Brownout detector was triggered
```

that is this. Fit the capacitor, and use a supply that can actually deliver 2A.
The CP2102's 5V pin is fine for flashing but marginal for streaming.

**`GPIO16` must stay unconnected** — it is the PSRAM chip select.

### 7.1 Testing with no capacitor

You do not need the capacitor to get through §8. The sketch ships with

```cpp
const bool LOW_POWER_BENCH = true;
```

in section 2, which attacks the current spike at the source rather than
smoothing it afterwards:

| Change | Effect |
|---|---|
| Pixel clock 20 MHz → 10 MHz | Roughly halves sensor draw. Costs frame rate, which does not matter for a stationary QR |
| Frame 640×480 → 400×296 | Smaller buffers, shorter transmit bursts |
| JPEG quality 10 → 14 | Smaller files, less radio airtime |
| Wi-Fi TX power → 11 dBm | The big one. Roughly halves peak current, and 11 dBm is ample at normal indoor range |

Together that is normally enough to run off a CP2102's USB 5V with nothing
fitted. The serial banner tells you when it is active.

Also helps, and costs nothing:

- Use the **shortest** jumper wires you have between adapter and camera
- Use a **rear-panel** USB port on a desktop, or a powered hub — front-panel
  and keyboard ports are often current-limited
- Test within a few metres of the router — weak signal means longer transmits

**Turn `LOW_POWER_BENCH` off once you have the capacitor.** 400×296 shortens
useful QR range considerably; you want the full 640×480 on the car.

If you can salvage a capacitor, anything **100µF or larger at 6.3V or more**
helps a great deal — it does not have to be exactly 470µF. Old phone chargers,
dead motherboards and scrap LED drivers are full of them.

> The capacitor stops being optional when the camera moves to the car. There
> the supply is an LM2596 buck converter whose feedback loop needs ~100µs to
> respond to a load step, with wire inductance in between. `LOW_POWER_BENCH`
> will not save you from that.

---

## 8. Manual tests, in order

Each test only depends on the ones before it. Stop at the first failure.

### Test 1 — status endpoint

Phone or laptop on the same network, browser to:

```
http://robotcam.local/status
```

Expect raw JSON:

```json
{"framesize":"640x480","psram":true,"heap":174320,"rssi":-47,
 "frames":0,"fails":0,"uptime":31,"ip":"192.168.0.53"}
```

If `robotcam.local` does not resolve, use the IP from §6 instead —
`http://192.168.0.53/status`. mDNS is a convenience, not a requirement; some
Android builds do not resolve `.local` names.

**This passing means Wi-Fi, HTTP and the board are all fine.** Any failure past
this point is camera or browser, not network.

### Test 2 — one still photo

```
http://robotcam.local/capture
```

You get a single JPEG. Refresh for a new one. Point the camera at something
with writing on it and confirm the text is legible.

### Test 3 — the live view page on your phone

```
http://robotcam.local/
```

This is the page you asked for. You should see:

- the live feed filling the width of the screen
- a **green dot** and the word `stream` in the corner
- a stats table below with resolution, signal and frame count
- **Client FPS** climbing to roughly 10–20

Four buttons:

| Button | What it does |
|---|---|
| **Snapshot mode** | Switches from MJPEG streaming to polling still frames. Use this if the stream freezes on your phone — some iOS builds kill long-lived MJPEG connections |
| **Pause** | Stops requesting frames. Frees the camera and saves battery |
| **Flash** | Pulses the white LED for ~1 second |
| **Rotate** | Rotates the picture 90° in the browser only. Use it to work out which way up the camera is mounted, then set `CAM_VFLIP` / `CAM_HMIRROR` in section 2 of the sketch so it is correct for everyone |

If the dot is **red** and says `reconnecting`, the page loaded but the stream on
port 81 did not. Press **Snapshot mode** — if that works, port 81 is being
blocked and you should stay in snapshot mode.

### Test 4 — the flash LED

Press **Flash**. The white LED on the front should light for about a second.

Also works directly: `http://robotcam.local/flash?on=1` and `?on=0`.

> Only ever pulse it. Left on, it gets hot enough to hurt and drags the 5V rail
> down. For QR scanning it is usually counterproductive — it reflects off the
> paper straight back into the lens. Print QR codes on **matte** paper and rely
> on room lighting.

### Test 5 — QR readability (the real acceptance test)

This is what the camera actually exists for.

1. Print a QR encoding exactly `BED1` at **40×40 mm or larger**, on matte paper.
   Any online QR generator works. Short payloads only — no URLs.
2. Hold it 15–25 cm from the lens, square on. While `LOW_POWER_BENCH` is on the
   frame is 400×296, so work at **10–15 cm** instead — the full range comes back
   when you switch to 640×480.
3. On the phone page, press **Snapshot mode** and look closely, or open
   `/capture` and zoom right in.

**Pass:** individual QR squares have clean, distinct edges. Not grey, not
smeared, no white blowout across the middle.

**Fail and what to do:**

| What you see | Fix |
|---|---|
| Blurry at every distance | The OV2640 lens screws in and out to focus. Break the small glue seal on the lens barrel and rotate it slowly by a few degrees until sharp |
| Sharp in the middle, soft at the edges | Normal. Centre the QR in frame |
| White glare washing out the code | Matte paper, angle the QR ~10° off perpendicular, do not use the flash |
| Too dark | More room light. Only then try `QR_GRAYSCALE = true` in section 2 |
| Fine at 10 cm, mush at 25 cm | Print the QR larger. 40 mm is the minimum, 50 mm is safer |

Get this right on the bench now. Once the camera is bolted to a moving chassis
it is much harder to diagnose.

---

## 9. Letting the laptop serve the control page

The board addresses are already set (§4). What is left is making sure the
laptop's Flask page is reachable from a phone on the same network.

This section only matters once you start `pc/app.py`. The camera itself needs
none of it.

### 9.1 Windows Firewall

This one catches everybody. Windows blocks inbound connections to Python on any
network it has classified as Public, and the symptom is precise and misleading:
the ESP32s connect fine, `robotcam.local` works from the phone, but the Flask
page on port 5000 just times out.

Two fixes, either works:

- When Windows first prompts *"Allow Python to communicate on these networks"*,
  tick **both Private and Public** and accept. If you have already dismissed
  it, go to `Windows Security → Firewall & network protection → Allow an app
  through firewall`, find Python, and tick both boxes.
- Or set the network itself to Private: `Settings → Network & internet → Wi-Fi
  → <your network> → Network profile type → Private`.

`app.py` already binds `0.0.0.0`, so nothing needs changing on the Flask side.

### 9.2 Reaching the page

The laptop uses `http://127.0.0.1:5000`.

A phone needs the **laptop's own address on the network**, which the router
assigns by DHCP and which is *not* one of the `.51`/`.52`/`.53` board
addresses. Find it with `ipconfig` — the `IPv4 Address` under
`Wireless LAN adapter Wi-Fi`:

```
http://<laptop-ipv4>:5000
```

That address can change when the laptop reconnects. If you get tired of
re-checking it, give the laptop a DHCP reservation in your router, or move to
the laptop hotspot where it is always `192.168.137.1`.

### 9.3 If the addresses do not match

If `ipconfig` showed a gateway other than `192.168.0.1`, change the third octet
in all four files — `esp32-cam.ino`, `car.ino`, `dispenser.ino` and
`pc/app.py` — keeping `.51` / `.52` / `.53` as the last octet. Keep the last
octet above 50 so it cannot collide with an address the router's DHCP pool
hands out.

---

## 10. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| Nothing on serial, board seems dead | `IO0` still jumpered to GND | Remove the jumper, press RST |
| Serial prints garbage | Wrong baud rate | Set the monitor to 115200 |
| `Failed to connect to ESP32: Timed out` | Not in bootloader mode | `IO0→GND`, press RST, *then* Upload |
| `Camera init FAILED 0x105` | Camera not detected | Reseat the ribbon cable — lift the black latch, insert fully, press the latch down. Check the 5V rail |
| `Camera init FAILED 0x101` | Out of memory | PSRAM not detected. Lower `FRAME_SIZE` to `FRAMESIZE_CIF` |
| `Brownout detector was triggered` | Supply sag | Confirm `LOW_POWER_BENCH = true` (§7.1), shorter wires, rear USB port. Then fit the 470µF cap and a 2A supply |
| Boots, prints IP, then reboot-loops | Same as above, triggered by the Wi-Fi radio | Same fix |
| `Wi-Fi FAILED`, restarting | 5 GHz network, or wrong SSID/password | Confirm the network is 2.4 GHz; SSID and password are case sensitive |
| Connects, but `robotcam.local` does not load | mDNS unsupported on that phone | Use the raw IP from serial |
| Page loads, video does not | Port 81 blocked or MJPEG unsupported | Press **Snapshot mode** |
| Stream freezes after ~30 s | Two clients fighting for the camera | Only one device streams at a time. Press Pause on the other |
| Stream is slow and jerky | Weak signal or too large a frame | Check `Signal` on the page. Drop `FRAME_SIZE` to `FRAMESIZE_CIF` |
| Picture upside down | Mounting orientation | Set `CAM_VFLIP = 1` in section 2 |
| Sketch too big | Wrong partition scheme | `Tools → Partition Scheme → Huge APP` |

---

## 11. Endpoint reference

| Endpoint | Port | Returns |
|---|---|---|
| `/` | 80 | Live view page (HTML) |
| `/capture` | 80 | One JPEG. `?flash=1` pulses the LED for the shot |
| `/status` | 80 | JSON diagnostics |
| `/flash?on=1` | 80 | White LED on. `?on=0` off |
| `/stream` | 81 | MJPEG multipart stream |

`/capture` is the one `pc/app.py` uses for QR decoding. `/stream` is what the
nurse manual-verify panel embeds. Everything else is for you.

---

## 12. What is next

The camera is done once §8 passes. The remaining camera-related work lives
elsewhere:

- QR decoding on the PC — `pc/app.py`, needs `pip install pyzbar pillow`
- Marker layout and QR placement — [`docs/07-track-and-navigation.md`](../../docs/07-track-and-navigation.md)
- Mounting height and angle — [`docs/04-chassis-and-mechanical.md`](../../docs/04-chassis-and-mechanical.md)
