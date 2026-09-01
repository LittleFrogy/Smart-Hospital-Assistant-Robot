#!/usr/bin/env python3
# ============================================================================
#  Smart Hospital Assistant Robot  --  PC control application
# ----------------------------------------------------------------------------
#  Runs on your PC. Hosts one web page that controls the whole system and does
#  all QR decoding. Talks to the three ESP32 boards over Wi-Fi HTTP.
#
#  FLOW
#    1. You pick a bed and click "Start delivery".
#    2. This app tells the dispenser to /drop and the car to /go?bed=N.
#    3. The app polls the car's /status. When the car reports SCANNING,
#       it pulls a frame from the camera, decodes the QR, and tells the car:
#          target bed   -> /deliver
#          wrong bed    -> /resume
#          TURNAROUND    -> /spin
#          STATION       -> /home
#    4. If the QR can't be read, the page switches to MANUAL mode: it shows
#       the live camera stream and Confirm / Wrong-bed buttons for a nurse.
#
#  RUN
#    pip install flask requests pyzbar pillow
#    python app.py
#    then open http://127.0.0.1:5000 in a browser
#
#  On Windows pyzbar needs the Visual C++ Redistributable.
#  On Linux:  sudo apt install libzbar0
# ============================================================================

from flask import Flask, jsonify, request, Response
import requests
import threading
import time
import io

# ---- optional QR decoding -------------------------------------------------
# If pyzbar/Pillow aren't installed, the app still runs in manual-only mode.
try:
    from pyzbar.pyzbar import decode as qr_decode
    from PIL import Image
    QR_AVAILABLE = True
except Exception:
    QR_AVAILABLE = False

# ---------------------------------------------------------------------------
# 1. CONFIG  --  set these to match your boards
# ---------------------------------------------------------------------------
# Laptop's Windows Mobile Hotspot: always 192.168.137.x, laptop itself at .1
CAR_IP  = "192.168.137.51"
DISP_IP = "192.168.137.52"
CAM_IP  = "192.168.137.53"

TARGET_STATION   = "STATION"
TARGET_TURNAROUND = "TURNAROUND"
BED_PREFIX       = "BED"          # QR payloads look like BED1, BED2, ...

POLL_INTERVAL_S  = 0.5            # how often the background loop polls the car
DECODE_ATTEMPTS  = 3             # tries before falling back to manual
DECODE_GAP_S     = 1.0
HTTP_TIMEOUT_S   = 4

app = Flask(__name__)

# ---------------------------------------------------------------------------
# 2. SHARED STATE (guarded by a lock)
# ---------------------------------------------------------------------------
lock = threading.Lock()
mission = {
    "active":      False,   # a delivery is running
    "target_bed":  0,
    "car_state":   "?",
    "last_marker": "",      # last decoded QR
    "mode":        "idle",  # idle | auto | manual
    "message":     "Idle.",
}

# ---------------------------------------------------------------------------
# 3. LOW-LEVEL BOARD CALLS
# ---------------------------------------------------------------------------
def car_get(path):
    return requests.get(f"http://{CAR_IP}{path}", timeout=HTTP_TIMEOUT_S)

def disp_get(path):
    return requests.get(f"http://{DISP_IP}{path}", timeout=HTTP_TIMEOUT_S)

def car_status():
    try:
        return car_get("/status").json()
    except Exception:
        return None

def read_marker(attempts=DECODE_ATTEMPTS):
    """Pull frames from the camera and try to decode a QR. Returns the string
    payload, or None if nothing decodes (or QR libs are unavailable)."""
    if not QR_AVAILABLE:
        return None
    for _ in range(attempts):
        try:
            raw = requests.get(f"http://{CAM_IP}/capture", timeout=HTTP_TIMEOUT_S).content
            codes = qr_decode(Image.open(io.BytesIO(raw)))
            if codes:
                return codes[0].data.decode("utf-8", "ignore").strip()
        except Exception:
            pass
        time.sleep(DECODE_GAP_S)
    return None

def dispatch(marker, target_bed, returning):
    """Given a decoded marker, tell the car what to do. Returns a short note."""
    m = (marker or "").upper()
    if m == TARGET_STATION:
        if returning:
            car_get("/home");   return "STATION -> home"
        else:
            car_get("/resume"); return "STATION (outbound) -> resume"
    if m == TARGET_TURNAROUND:
        car_get("/spin");       return "TURNAROUND -> spin"
    if m == f"{BED_PREFIX}{target_bed}":
        car_get("/deliver");    return f"{m} == target -> deliver"
    if m.startswith(BED_PREFIX):
        car_get("/resume");     return f"{m} != target -> resume"
    # unrecognised payload
    return None

# ---------------------------------------------------------------------------
# 4. BACKGROUND MISSION LOOP
#    Runs in its own thread. Polls the car; when it is SCANNING, decodes and
#    dispatches. Falls back to manual mode if decoding fails.
# ---------------------------------------------------------------------------
def mission_loop():
    handled_scan = False   # so we act once per SCANNING episode
    while True:
        time.sleep(POLL_INTERVAL_S)
        st = car_status()
        if st is None:
            with lock:
                mission["car_state"] = "unreachable"
            continue

        with lock:
            mission["car_state"] = st.get("state", "?")
            active     = mission["active"]
            target_bed = mission["target_bed"]

        returning = bool(st.get("returning", False))
        cstate    = st.get("state", "?")

        if cstate != "SCANNING":
            handled_scan = False
            # mission ends when the car is home and idle again
            if active and cstate == "STANDBY":
                with lock:
                    mission["active"]  = False
                    mission["mode"]    = "idle"
                    mission["message"] = "Delivery complete. Car at station."
            continue

        # car is SCANNING
        if handled_scan:
            continue                       # already acting on this stop

        with lock:
            mission["mode"] = "auto"
            mission["message"] = "Scanning marker..."

        marker = read_marker()
        if marker is None:
            # couldn't decode -> hand over to the nurse
            with lock:
                mission["mode"] = "manual"
                mission["last_marker"] = ""
                mission["message"] = "QR unreadable. Nurse, please verify."
            handled_scan = True            # stop auto-acting; wait for manual
            continue

        note = dispatch(marker, target_bed, returning)
        with lock:
            mission["last_marker"] = marker
            if note:
                mission["message"] = note
                handled_scan = True
            else:
                # decoded something we don't recognise -> manual
                mission["mode"] = "manual"
                mission["message"] = f"Unrecognised code '{marker}'. Verify."
                handled_scan = True

# ---------------------------------------------------------------------------
# 5. WEB ROUTES
# ---------------------------------------------------------------------------
@app.route("/")
def index():
    return PAGE

@app.route("/api/start", methods=["POST"])
def api_start():
    bed = int(request.json.get("bed", 1))
    try:
        disp_get("/drop")               # start the medicine dropping
    except Exception:
        pass                            # dispenser optional during early tests
    try:
        r = car_get(f"/go?bed={bed}")
        ok = (r.status_code == 200)
    except Exception:
        ok = False
    with lock:
        mission.update(active=ok, target_bed=bed, mode="auto",
                       message=("Delivery started." if ok else "Car unreachable."))
    return jsonify(ok=ok)

@app.route("/api/manual", methods=["POST"])
def api_manual():
    """Nurse pressed Confirm or Wrong-bed on the manual panel."""
    action = request.json.get("action", "")
    if action == "confirm":
        car_get("/deliver"); msg = "Manual: delivered."
    elif action == "wrong":
        car_get("/resume");  msg = "Manual: wrong bed, resuming."
    elif action == "spin":
        car_get("/spin");    msg = "Manual: spinning."
    elif action == "home":
        car_get("/home");    msg = "Manual: home."
    else:
        return jsonify(ok=False)
    with lock:
        mission["mode"] = "auto"
        mission["message"] = msg
    return jsonify(ok=True)

@app.route("/api/abort", methods=["POST"])
def api_abort():
    try: car_get("/abort")
    except Exception: pass
    with lock:
        mission["message"] = "ABORT sent. Car returning."
    return jsonify(ok=True)

@app.route("/api/state")
def api_state():
    with lock:
        data = dict(mission)
    data["cam_ip"] = CAM_IP
    data["qr_available"] = QR_AVAILABLE
    return jsonify(data)

# ---------------------------------------------------------------------------
# 6. THE PAGE  (single file, no template folder needed)
# ---------------------------------------------------------------------------
PAGE = """<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Hospital Robot Control</title>
<style>
 body{font-family:system-ui,Arial,sans-serif;margin:0;background:#f4f5f7;color:#1f2430}
 header{background:#1f2937;color:#fff;padding:14px 20px;font-size:18px;font-weight:700}
 .wrap{max-width:820px;margin:20px auto;padding:0 16px}
 .card{background:#fff;border:1px solid #e3e5ea;border-radius:12px;padding:18px;margin-bottom:16px}
 h2{margin:0 0 12px;font-size:15px;color:#374151}
 button{font-size:15px;padding:10px 16px;border-radius:8px;border:1px solid #cfd3da;
        background:#fff;cursor:pointer;margin:4px 6px 4px 0}
 button.primary{background:#2563eb;color:#fff;border-color:#2563eb}
 button.danger{background:#b91c1c;color:#fff;border-color:#b91c1c}
 button.ok{background:#0f7a4f;color:#fff;border-color:#0f7a4f}
 select{font-size:15px;padding:9px;border-radius:8px;border:1px solid #cfd3da}
 .row{display:flex;align-items:center;gap:10px;flex-wrap:wrap}
 .state{font-weight:700}
 .msg{margin-top:8px;color:#374151}
 .pill{display:inline-block;padding:3px 10px;border-radius:999px;font-size:13px;
       background:#eef1f5;color:#374151}
 #manual{display:none}
 img{max-width:100%;border-radius:10px;border:1px solid #e3e5ea;margin-top:8px}
 .muted{color:#8a8f98;font-size:13px}
</style></head><body>
<header>Smart Hospital Assistant Robot — Control</header>
<div class="wrap">

 <div class="card">
  <h2>Start a delivery</h2>
  <div class="row">
   <label>Bed:
    <select id="bed">
      <option value="1">Bed 1</option>
    </select>
   </label>
   <button class="primary" onclick="startDelivery()">Start delivery</button>
   <button class="danger" onclick="abort()">Abort</button>
  </div>
 </div>

 <div class="card">
  <h2>Status</h2>
  <div>Car: <span class="state" id="cstate">—</span>
       <span class="pill" id="mode">idle</span></div>
  <div>Last marker: <span id="marker">—</span></div>
  <div class="msg" id="msg">—</div>
  <div class="muted" id="qrnote"></div>
 </div>

 <div class="card" id="manual">
  <h2>Manual verify</h2>
  <div class="muted">Automatic decode failed. Check the patient on the live view, then confirm.</div>
  <img id="stream" src="" alt="camera stream">
  <div class="row" style="margin-top:10px">
   <button class="ok" onclick="manual('confirm')">Confirm &amp; deliver</button>
   <button onclick="manual('wrong')">Wrong bed — continue</button>
   <button onclick="manual('spin')">This is the turn zone</button>
   <button onclick="manual('home')">This is the station</button>
  </div>
 </div>

</div>
<script>
let camIp = "";
async function startDelivery(){
  const bed = +document.getElementById('bed').value;
  await fetch('/api/start',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({bed})});
}
async function abort(){ await fetch('/api/abort',{method:'POST'}); }
async function manual(action){
  await fetch('/api/manual',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({action})});
}
async function poll(){
  try{
    const s = await (await fetch('/api/state')).json();
    document.getElementById('cstate').textContent = s.car_state;
    document.getElementById('mode').textContent   = s.mode;
    document.getElementById('marker').textContent = s.last_marker || '—';
    document.getElementById('msg').textContent    = s.message;
    camIp = s.cam_ip;
    document.getElementById('qrnote').textContent =
       s.qr_available ? '' : 'pyzbar not installed — manual verify only.';
    const man = document.getElementById('manual');
    if(s.mode === 'manual'){
      man.style.display = 'block';
      const img = document.getElementById('stream');
      if(!img.src) img.src = 'http://' + camIp + ':81/stream';
    } else {
      man.style.display = 'none';
      document.getElementById('stream').src = '';
    }
  }catch(e){}
}
setInterval(poll, 700); poll();
</script>
</body></html>"""

# ---------------------------------------------------------------------------
# 7. START
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    t = threading.Thread(target=mission_loop, daemon=True)
    t.start()
    print("Control panel: http://127.0.0.1:5000")
    print(f"Boards -> car {CAR_IP}, dispenser {DISP_IP}, cam {CAM_IP}")
    print("QR decoding:", "ON" if QR_AVAILABLE else "OFF (manual only)")
    app.run(host="0.0.0.0", port=5000, threaded=True)
