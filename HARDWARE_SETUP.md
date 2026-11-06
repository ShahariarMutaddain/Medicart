# MediCart — hardware setup (ESP32 → Arduino → Raspberry Pi)

Website: https://medicart-iub.vercel.app (console) · https://medicart-iub.vercel.app/cart (cart screen)
Firebase project: medicart-e20e0 · MQTT: HiveMQ public, prefix `iub-medicart-591a7e01d2`

## What is in this kit

| Folder / file | Goes to |
|---|---|
| `index.html`, `cart.html`, `vercel.json` | GitHub → Vercel (already done) |
| `firebase/` | Rules, pasted in the Firebase console (already done) |
| `esp32_sensor/prebuilt/medicart_sensor_v2.1.0_merged.bin` | ESP32, flashed at `0x0` |
| `arduino/medicart/medicart.ino` | Arduino Mega: firmware 2.1 = Rifat's firmware + manual drive (`[CTRL-2.1]`); `medicart.cpp.original` is the untouched copy |
| `MideCart/` | Raspberry Pi home folder → `/home/<user>/MideCart` (Rifat's vision code + YOLO models) |
| `raspberry_pi/` | Raspberry Pi home folder → run `sudo ./install.sh` |

## Part 1 · ESP32 + MAX30105

Wiring: VIN→**3V3**, GND→GND, SDA→**GPIO25**, SCL→**GPIO26**, INT→**GPIO33**.

1. Chrome → https://espressif.github.io/esptool-js/ → Baudrate 460800 → **Connect** → choose the ESP32 port
   (no port → install the CP210x or CH340 driver; stuck → hold **BOOT** while connecting).
2. **Erase Flash**. Then Flash Address **`0x0`** + file `esp32_sensor/prebuilt/medicart_sensor_v2.1.0_merged.bin` → **Program** → press **EN**.
3. LED blinks fast → phone joins **`MediCart-Sensor-XXXX`** (password `medicart`) → page opens (or http://192.168.4.1).
4. Admin password `medicart-admin` → your **2.4 GHz** Wi-Fi + password → **Advanced** → new admin password → **Save & connect**.
5. LED solid = online. Console shows **Sensor streaming**; finger on sensor → HR and SpO₂.
   Change Wi-Fi later: hold BOOT 5 s while running.

## Part 2 · Arduino Mega (wheels lifted!)

1. Arduino IDE 2 → File → Open → `arduino/medicart/medicart.ino`.
2. Tools → Board → **Arduino Mega or Mega 2560**, Processor **ATmega2560**, Port `/dev/cu.usbmodem…` → **Upload**.
3. Serial Monitor **115200**, **Newline** → `MediCart ready`, then `FW medicart-ctrl 2.1`.
4. Watchdog test: type `SAFE` several times → wheels turn; stop typing → they stop within 2 s. Pull USB while turning → stop within 2 s.

## Part 3 · Raspberry Pi (Raspberry Pi OS 64-bit desktop, Bookworm)

### 3.1 SD card
Raspberry Pi Imager → your Pi → **Raspberry Pi OS (64-bit)** → SD card → Edit settings: hostname `medicart`, user `pi` + password,
Wi-Fi + country **BD**, time zone Asia/Dhaka, Services → **Enable SSH** → write.
Keep the old Ubuntu card until anything on it is copied off.

### 3.2 First boot
Monitor on **HDMI 0** (port next to the USB-C power). Plug in webcam, speaker, mic, keyboard, mouse, Arduino USB → power on.
```bash
sudo apt update && sudo apt full-upgrade -y && sudo reboot
```

### 3.3 Copy the kit to the Pi
USB stick: copy `MideCart` and `raspberry_pi` into the Pi's home folder (`/home/pi`). Or from the Mac:
```bash
cd ~/Downloads/medicart-system
scp -r MideCart raspberry_pi pi@medicart.local:~/
```
Result on the Pi:
```
/home/pi/MideCart/yolov8n.pt
/home/pi/MideCart/yolov8n-pose.pt
/home/pi/MideCart/vision/medicart.py
/home/pi/raspberry_pi/install.sh
```
(`medicart.py` loads the models from the folder ABOVE it — keep this layout.)

### 3.4 Python environment for medicart.py (10–20 min)
```bash
cd ~/MideCart
python3 -m venv venv
venv/bin/pip install --upgrade pip
venv/bin/pip install ultralytics opencv-python-headless pyserial numpy
```
Quick test (camera still free at this point; Ctrl+C to stop):
```bash
cd ~/MideCart/vision && ~/MideCart/venv/bin/python medicart.py
```
Look for `[INFO] Connected to /dev/ttyACM0 at 115200 baud`. Serial "permission denied" → `sudo usermod -aG dialout pi` → reboot.

### 3.5 Install the cart package
```bash
cd ~/raspberry_pi && chmod +x install.sh && sudo ./install.sh
```
| Question | Answer |
|---|---|
| Cart page URL | `https://medicart-iub.vercel.app/cart` |
| Admin password for the Wi-Fi setup page | your own, 8+ characters |
| Password of the setup hotspot | your own, 8+ characters |
| Path to medicart.py | `/home/pi/MideCart/vision/medicart.py` |
| Python for medicart.py | `/home/pi/MideCart/venv/bin/python` |
| Install the drive controller? | `y` |
| Cart account email | `cart01@medicart.iub` |
| Cart account password (hidden) | the cart account's password (stays on the Pi, root only) |
| Firebase apiKey / databaseURL | press Enter (defaults = medicart-e20e0) |

Then `sudo reboot`.

### 3.6 First start
1. Screen shows **Sign in this cart** → `cart01@medicart.iub` + its password → **Sign in & start** (remembered).
2. Hold the faint **gear** (bottom-right) 1.5 s → Microphone, Speaker → **Test speaker** → Close. Camera = `MediCart-Call`.
3. Console on the laptop shows **Cart online**.

### 3.7 Checks
```bash
v4l2-ctl --list-devices               # MediCart-Vision /dev/video10 + MediCart-Call /dev/video11
systemctl status medicart-camera medicart-vision medicart-wifi-portal
journalctl -u medicart-vision -f      # live output of medicart.py
```

### 3.8 New Wi-Fi
No known Wi-Fi within 45 s of boot → hotspot **`MediCart-Cart-Setup`** → phone joins → http://10.42.0.1 →
admin password → network → **Connect MediCart**. On purpose: `sudo medicart-wifi-setup`.

## Part 4 · Full test (wheels lifted first, then floor)
1. Finger on sensor → vitals on cart + console.
2. Console → Consult → Start video consultation → cart answers after 3 s (repeat on mobile data = TURN test).
3. Ask the patient → Finger on sensor → bilingual prompt on the cart.
4. End call → Records → Consultations.
5. Step in front of the camera while line-following → robot stops.
6. `sudo systemctl stop medicart-vision` → motors stop within 2 s → `sudo systemctl start medicart-vision`.
7. Console → **Drive** → **Manual** → hold **W**: the cart drives; let go: it stops within 1 s. **Emergency stop** → stays stopped; **Auto** → line following again. Over SSH: `medicart-drive`. Details: `CONTROL_SYSTEM.md`.

## Troubleshooting
| Problem | Fix |
|---|---|
| Black screen after the rainbow | HDMI 0; Raspberry Pi OS card (not Ubuntu) |
| "NetworkManager is not running" | `sudo raspi-config` → Advanced → Network Config → NetworkManager → reboot |
| v4l2loopback failed | `sudo apt full-upgrade -y && sudo reboot`, run install.sh again |
| medicart-vision: camera busy | must read camera 10 — check `/etc/medicart/medicart.env` (`MEDICART_VIDEO_SOURCE=10`) |
| Cart offline / white screen | URL in `/etc/medicart/kiosk.conf` must be `https://medicart-iub.vercel.app/cart` |
| Sensor offline, LED solid | topic prefix changed — keep `iub-medicart-591a7e01d2` |
| No sound in calls | gear → choose Speaker → Test speaker |
