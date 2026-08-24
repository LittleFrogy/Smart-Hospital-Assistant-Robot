# Bill of Materials

## 1. Bill of materials

### 1.1 Car

| Item | Qty | Purpose |
|---|---|---|
| ESP32 DevKit V1 (30-pin, CP2102, USB-C) | 1 | Main controller |
| ESP32-CAM (AI-Thinker) | 1 | QR capture |
| 470µF 16V electrolytic | 2 | One at ESP32-CAM, one at lock servo |
| L298N motor driver module | 1 | Dual H-bridge |
| TT gear motors + 2WD chassis kit | 1 set | Drive |
| **Ball caster** (not swivel) | 1 | Front support — see §6.2 |
| 5-channel IR line array (digital) | 1 | Line following + marker detection |
| HC-SR04 ultrasonic sensor | 1 | Obstacle stop |
| 16×2 LCD + I2C backpack (PCF8574) | 1 | Status display |
| BSS138 4-channel level shifter | 1 | I2C to the 5V LCD, plus servo signal |
| SG90 servo | 1 | Compartment lock |
| Active buzzer | 1 | Audible alerts |
| S8050 (or 2N2222) NPN transistor | 1 | Buzzer drive |
| 1N4148 diode | 1 | Buzzer flyback |
| LED | 1 | Status |
| Resistors: 1kΩ ×2, 2kΩ ×1, 220Ω ×2 | 5 total | See §3.4 |
| LM2596 adjustable buck module | 2 | 7.5V motor rail, 5.0V logic rail |
| 3S LiPo pack | 1 | Power |
| 5A blade fuse + holder | 1 | Short-circuit protection |
| Power switch | 1 | Main disconnect |
| 3S LiPo low-voltage alarm | 1 | Cell protection |
| 1000µF 16V electrolytic | 1 | L298N bulk decoupling |
| 100nF ceramic capacitor | 6 | Per-module + motor brush noise |

### 1.2 Dispenser

| Item | Qty | Purpose |
|---|---|---|
| ESP32 DevKit V1 (30-pin) | 1 | Controller |
| SG90 servo | 1 | Drop gate |
| 470µF 16V electrolytic | 1 | Servo inrush |
| 5V 2A USB adapter | 1 | Power — preferred, see §5.6 |
| *or* LM2596 + 3S LiPo + fuse + alarm | 1 set | Only if untethered |
| Momentary push button + 100nF | 1 | *Optional* Wi-Fi-down backup |

### 1.3 Bench equipment

| Item | Notes |
|---|---|
| **CP2102 USB-TTL adapter** | Mandatory — the ESP32-CAM has no USB port. Choose CP2102 over FT232RL: cheaper, widely stocked, and the DevKit already uses a CP2102 so the driver is already installed. Must have a 5V output pin and a 3.3V/5V selector jumper. |
| 3S LiPo balance charger | A plain wall-wart will damage LiPo packs |
| Multimeter | Non-negotiable — buck calibration and the ECHO divider check both require it |
| USB-C cable | The DevKit is USB-C, not micro-USB |

### 1.4 Why exactly five resistors

| Value | Where | Can a level shifter replace it? |
|---|---|---|
| 1kΩ + 2kΩ | HC-SR04 ECHO divider | Technically yes, but **don't** — the BSS138's pull-up slows the signal edge, and ECHO timing is what your distance reading depends on |
| 1kΩ | S8050 base | No. Unrelated to level shifting. |
| 220Ω ×2 | Status LED + spare | No. Current limiting is mandatory. |

### 1.5 Approximate cost (BDT)

| Category | Cost |
|---|---|
| Controllers (2 DevKit + 1 CAM) | ~2,300 |
| Chassis, motors, ball caster, driver | ~1,050 |
| Sensors (IR array, HC-SR04) | ~450 |
| Display, servos, level shifter | ~700 |
| Power (packs, bucks, charger, fuses, alarms) | ~3,700 |
| Passives, USB-TTL adapter, misc | ~600 |
| **Total** | **≈ 8,300–9,000** |
