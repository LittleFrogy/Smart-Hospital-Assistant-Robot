# PC control layer

Flask app that hosts the control page and talks to all three boards.

```
Browser ──HTTP──> Flask ──HTTP──> Car ESP32
                        ├──HTTP──> Dispenser ESP32
                        └──HTTP──> ESP32-CAM
```

## Dependencies

```
pip install flask requests pyzbar pillow
```

On Windows, `pyzbar` needs the Visual C++ Redistributable. On Linux: `sudo apt install libzbar0`.

## Responsibilities

1. Serve the control page — bed selection, dispense button, abort button
2. Poll the car's `/status`
3. When the car reports `SCANNING`, pull `/capture` from the camera and decode
4. Dispatch on the decoded string:

| Decoded | Action |
|---|---|
| Matches target bed | POST `/unlock` to the car |
| Different bed | POST `/resume` to the car |
| `TURNAROUND` | Car handles it locally — no action |
| `STATION` while returning | Car handles it locally — no action |
| Nothing after 3 attempts | Switch the page to manual verify mode |

## Manual verify mode

Show the live MJPEG stream, the expected bed number, and two buttons: **Confirm and unlock** and **Wrong bed — continue**. This is the intended fallback, not a workaround — real clinical systems have manual overrides.

## Decoder

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

Use one attempt instead of three on the return leg — the car only needs to find `STATION`, so a fast miss is what you want.

## Config

Keep the three static IPs in one place at the top of the app. Set them with `WiFi.config()` on the boards too, or DHCP will reassign them after a router reboot.

Full API: [08 — Software and API](../docs/08-software-and-api.md).
