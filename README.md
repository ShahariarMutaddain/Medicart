# MediCart — IUB Robotics Team

Unified system for the MediCart autonomous medical cart:

- **Doctor console** (web): sign-in, live heart rate / SpO₂, trends, video consultation with the patient, consultation notes and records.
- **Cart screen** (runs on the Raspberry Pi): patient-facing kiosk in English + Bangla, answers the doctor's video call through the cart's webcam, mic and speaker, and logs readings to Firebase.
- **Sensor firmware** (ESP32 + MAX30105): heart rate and SpO₂ over MQTT, Wi-Fi set from a phone.
- **Raspberry Pi setup**: one camera shared by OpenCV and the video call, Wi-Fi setup hotspot, kiosk autostart, and the YOLO vision script as a boot service.

```
 Doctor's laptop/phone                          MediCart
┌──────────────────────┐                 ┌───────────────────────────────┐
│ Console (Vercel)     │◀── WebRTC ─────▶│ Cart screen (Chromium kiosk)  │
│  /                   │  video + audio  │  /cart  on the Raspberry Pi   │
└─────────┬────────────┘   peer-to-peer  └──────┬──────────────┬─────────┘
          │  Firebase: sign-in, call signalling, │              │ USB webcam + mic, speaker
          │  presence, records                   │              │
          ▼                                      ▼              │
   ┌──────────────┐        ┌─────────────────────────┐   ┌──────┴────────┐
   │   Firebase   │        │ HiveMQ public (MQTT/TLS)│◀──│ ESP32 +       │
   │ Auth · RTDB  │        │ <prefix>/01/sensors     │   │ MAX30105      │
   │ Firestore    │        │ <prefix>/01/status      │   └───────────────┘
   └──────────────┘        └─────────────────────────┘
                     Pi also runs medicart.py (YOLO) → Arduino Mega (motors)
```

**One camera, two users.** The Pi's single USB camera is opened only by `medicart-camera` (ffmpeg), which copies every frame to two virtual cameras:

```
USB camera ──▶ medicart-camera (ffmpeg) ──┬──▶ /dev/video10  "MediCart-Vision" ──▶ medicart.py (OpenCV / YOLO)
 /dev/video0     1280×720 MJPEG            └──▶ /dev/video11  "MediCart-Call"   ──▶ cart screen (video call)
```

Both get the same live picture at the same time, so obstacle detection keeps running during a call. `medicart.py` needs no code change — it only reads camera **10** instead of **0** (`MEDICART_VIDEO_SOURCE=10`, set for you by the installer).

---

## What's in this folder

| Path | What it is |
|---|---|
| `index.html`, `cart.html`, `js/`, `assets/`, `vercel.json` | The website, at the top of the repo so Vercel serves it with default settings. `index.html` = doctor console, `cart.html` = cart screen. |
| `js/config.js` | **The one file you edit**: Firebase project, MQTT broker, cart ID, TURN servers. |
| `firebase/` | Security rules for Realtime Database and Firestore. |
| `esp32_sensor/medicart_sensor/` | Arduino sketch for the ESP32 sensor board. |
| `esp32_sensor/prebuilt/` | Ready-to-flash firmware (`…_merged.bin`, flash at address `0x0`). |
| `raspberry_pi/` | Copy to the Pi and run `sudo ./install.sh`. |

---

## 1 · Firebase (≈10 min)

Open <https://console.firebase.google.com> → project **medicart-e20e0**.

1. **Authentication → Get started → Sign-in method → Email/Password → Enable → Save.**
2. **Authentication → Users → Add user**, once for each person/device:
   - every doctor, e.g. `rahman@iub.edu.bd` + a password
   - one **cart account**, e.g. `cart01@medicart.iub` + a strong password (the email doesn't need to be real)
3. **Authentication → Settings → User actions → untick “Enable create (sign-up)” → Save.**
   Your API key is public in the website, so this stops strangers from creating accounts. You can still add users from the console.
4. **Realtime Database → Rules** → replace everything with `firebase/database.rules.json` → **Publish**.
5. **Firestore Database** (click *Create database* if it isn't there yet — production mode, any nearby region) → **Rules** → replace with `firebase/firestore.rules` → **Publish**.

> Command-line alternative for steps 4–5:
> `npm i -g firebase-tools && firebase login && cd firebase && firebase deploy --only database,firestore:rules`

## 2 · MQTT broker — HiveMQ public (nothing to set up)

Readings travel over HiveMQ's **free public broker**. There's no account, no login and no expiry:

| | Sensor (ESP32) | Website |
|---|---|---|
| Host | `broker.hivemq.com` | `broker.hivemq.com` |
| Port | `8883` (MQTT over TLS) | `8884` (secure WebSocket, path `/mqtt`) |
| Login | none | none |

The broker is shared with everyone, so MediCart's topics sit under a long random prefix:

```
iub-medicart-591a7e01d2/01/sensors     ← readings, once a second
iub-medicart-591a7e01d2/01/status      ← online/offline, IP, Wi-Fi signal (retained)
```

Treat that prefix like a password: anyone who knows it can read the readings or send fake ones. It lives in two places, and they must match:
- `js/config.js` → `MQTT_TOPIC_PREFIX`
- `esp32_sensor/medicart_sensor/medicart_sensor.ino` → `TOPIC_PREFIX`

To change it, edit both, re-flash the ESP32 and redeploy the website.

**Check it with MQTTX** (desktop app, <https://mqttx.app>):
1. **New Connection** → Name `MediCart` → Host `mqtts://` `broker.hivemq.com` → Port `8883` → SSL/TLS **on** → leave username and password empty → **Connect**.
2. **New Subscription** → Topic `iub-medicart-591a7e01d2/#` → **Confirm**.
3. With the sensor powered, a message arrives every second. Put a finger on it and `heartRate` / `spo2` fill in.

> The public broker is fine for a university demo. It has no uptime guarantee and is not meant for private data. For real patients, move to a private broker (e.g. EMQX Serverless, free with spend limit 0): put its `wss://` URL and a login in `config.js`, and its host and login in the sensor's setup page (*Advanced*).

## 3 · Deploy the website to Vercel (≈5 min)

**Option A — Vercel CLI**

```bash
npm i -g vercel
cd medicart-system
vercel            # first time: "Set up and deploy?" → Y, new project, name e.g. medicart-iub,
                  # directory ./ , no build command, no output dir changes
vercel --prod
```

**Option B — GitHub**: push this folder to a repo → <https://vercel.com/new> → import it → **Root Directory: `./` (default)**, **Framework Preset: Other** → Deploy.

Before deploying you can also just double-click `index.html` to try the console locally.

You get two pages:

| Page | URL | Who opens it |
|---|---|---|
| Doctor console | `https://medicart-iub.vercel.app/` | Doctors, on any laptop or phone |
| Cart screen | `https://medicart-iub.vercel.app/cart` | Only the Raspberry Pi on the cart |

Optional: Firebase → **Authentication → Settings → Authorized domains → Add domain** → `medicart-iub.vercel.app` (needed later for Google sign-in or custom email links).

## 4 · ESP32 sensor

### Flash it — pick one

**A · From the browser (no Arduino IDE)**
1. Chrome or Edge → <https://espressif.github.io/esptool-js/> → **Connect** → pick the ESP32's USB port (hold **BOOT** if it won't connect).
2. Flash address **`0x0`**, file **`esp32_sensor/prebuilt/medicart_sensor_v2.1.0_merged.bin`** → **Program**.
3. Press **EN/RESET**.

**B · Arduino IDE**
1. Boards Manager → install **esp32 by Espressif Systems**.
2. Library Manager → install **PubSubClient** (Nick O'Leary) and **SparkFun MAX3010x Pulse and Proximity Sensor Library**.
3. Open `esp32_sensor/medicart_sensor/medicart_sensor.ino` → Board **ESP32 Dev Module** → Upload.

(Compiled and verified on esp32 core 2.0.17 with all warnings enabled: 73 % of flash.)

### Connect it to Wi-Fi from your phone

1. Power the sensor. The on-board LED blinks fast = setup mode.
2. Phone → Wi-Fi → join **`MediCart-Sensor-XXXX`**, password **`medicart`**.
3. The setup page opens by itself. If not, open **http://192.168.4.1**.
4. Admin password: **`medicart-admin`** → pick your network → its password.
5. Open **Advanced** → set a **New admin password** (6+ characters) → **Save & connect**.
6. LED solid = online. The console shows **Sensor streaming**.

**Change Wi-Fi later:** hold **BOOT** for 5 seconds while it's running (LED starts blinking fast), or open `http://medicart-sensor.local` / the IP shown on the console's **System** page.
If the saved network isn't there at power-up, the setup hotspot opens after 20 seconds. If Wi-Fi drops while running, it keeps retrying for 5 minutes, then restarts (and opens the hotspot if the network is still gone).

> The ESP32 joins **2.4 GHz WPA2-Personal** networks only. Campus networks that need a web login or a username (WPA2-Enterprise) won't work. Use a lab router or a phone hotspot.

## 5 · Raspberry Pi (cart screen + Wi-Fi setup + vision)

**You need:** Raspberry Pi 4 (4 GB+) or Pi 5 · **Raspberry Pi OS Bookworm 64-bit with desktop** · a screen (HDMI or touch) · speakers · **one USB webcam** (the same one YOLO uses) · a microphone (the webcam's own mic is fine).

> The installer builds the `v4l2loopback` kernel module, so the Pi needs internet and a few minutes the first time. Run `sudo apt full-upgrade` and reboot first if the Pi hasn't been updated in a while.

1. Copy the `raspberry_pi` folder to the Pi (USB stick, or `scp -r raspberry_pi pi@<pi-ip>:~/`).
2. On the Pi:
   ```bash
   cd ~/raspberry_pi
   sudo ./install.sh
   ```
   It asks for:
   - **Cart page URL** → `https://medicart-iub.vercel.app/cart`
   - **Admin password** for the Pi's Wi-Fi setup page
   - **Hotspot password** (8+ characters)
   - **Path to `medicart.py`** to start it at boot (leave blank to keep starting it yourself), then the Python of its venv
3. `sudo reboot`
4. **First start only:** the screen shows **Sign in this cart**. Plug in a keyboard and enter the **cart account** from step 1. It stays signed in after restarts.
5. **Check the call devices:** the screen picks the **MediCart-Call** camera by itself. Press and hold the faint **gear** (bottom-right) for 1.5 s to choose the **Microphone** and **Speaker** → **Test speaker** → **Close**.

### Shared camera — checks and settings

```bash
v4l2-ctl --list-devices              # shows MediCart-Vision (/dev/video10) and MediCart-Call (/dev/video11)
systemctl status medicart-camera     # should be "active (running)"
journalctl -u medicart-camera -f     # which camera and mode it opened
```

- Settings live in `/etc/medicart/camera.conf` (camera, `mjpeg`, `1280x720`, `30` fps). Keep the size `medicart.py` was calibrated at. If the camera refuses MJPEG 720p, the splitter falls back to raw video, then 640×480, by itself.
- Running `medicart.py` by hand instead of as a service? Start it with `MEDICART_VIDEO_SOURCE=10 python3 medicart.py`, or set `CAMERA_INDEX = 10` at the top of the file. Camera **0** is now held by the splitter, so opening it directly fails with “device busy”.
- Pi 4 running hot during calls? Set `CAMERA_FPS=15` in `camera.conf`, then `sudo systemctl restart medicart-camera`.

### Moving the cart to a new Wi-Fi

When the Pi boots and can't join a saved network within 45 s, it opens the hotspot **`MediCart-Cart-Setup`**:

1. Phone → join `MediCart-Cart-Setup` (your hotspot password) → the setup page opens (or **http://10.42.0.1**).
2. Admin password → network → password → **Connect MediCart**.
3. The hotspot closes and the Pi joins the network. If the password was wrong, the hotspot returns in about a minute with the error.

To open it on purpose (over SSH or keyboard): `sudo medicart-wifi-setup`

## 6 · Test it end to end

1. Laptop → console URL → sign in as a doctor. Top-right should read **LIVE**.
2. **Overview → Care loop:** You → MediCart-01 → Patient all green once the cart screen is on and a finger is on the sensor.
3. Place a fingertip on the MAX30105 → *Measuring 1–4 of 4* → *Reading complete* with HR and SpO₂, on both the cart and the console.
4. **Consult → Start video consultation** → allow camera/mic → the cart rings and **answers by itself after 3 s** (change in cart settings).
5. Try **Ask the patient → Finger on sensor**. The message appears on the cart in English and Bangla.
6. Type notes → **End call** (red button) → **Records → Consultations** shows the call with duration, vitals and notes.

No hardware yet? **Demo data** in the console's top bar simulates the sensor (nothing is saved).

## 7 · Troubleshooting

| Symptom | Fix |
|---|---|
| Stuck on the loading screen / **“MediCart couldn't start”** | The message says what failed. Usually no internet (Firebase loads from www.gstatic.com), or the `js/` folder isn't next to `index.html`. |
| Console says **Cart offline** | Cart screen not on or not signed in. On the Pi: is Chromium showing `/cart`? Check `/etc/medicart/kiosk.conf`. |
| Call rings, cart answers, then **Connection failed** | Doctor and cart are on networks that block direct connections (common on campus Wi-Fi ↔ mobile data). Add a TURN server to `ICE_SERVERS` in `js/config.js` (e.g. a free Metered.ca or Cloudflare TURN account) and redeploy. The console's stats chip shows `relay` when TURN is in use. |
| Cart shows **No camera** | `systemctl status medicart-camera`. Webcam unplugged, or `v4l2loopback` not loaded (`sudo modprobe v4l2loopback`, then restart the service). The cart screen picks the camera back up by itself within ~15 s. |
| `medicart.py`: “Could not open camera 0” / device busy | It is opening the real camera, which the splitter owns. Use camera **10** (see “Shared camera”). |
| `modprobe: FATAL: Module v4l2loopback not found` | Kernel headers didn't match: `sudo apt full-upgrade`, reboot, re-run `sudo ./install.sh`. |
| Cart screen picked the wrong camera | Hold the gear → *Telemedicine camera* → **MediCart-Call** (or *System default* to let it choose again). |
| Doctor can't hear the patient | Cart settings → Microphone. In the console, check the speaker button isn't switched off (white). |
| Patient can't hear the doctor | Cart settings → Speaker → *Test speaker*. On the Pi: `wpctl status` / desktop volume. |
| **Sensor offline** but ESP32 LED is solid | Topic prefix in `config.js` doesn't match the firmware's `TOPIC_PREFIX`, or the ESP32's broker was changed in its setup page → *Advanced* → host `broker.hivemq.com`, port `8883`, tick *Broker needs no login*. Check with MQTTX (section 2). |
| ESP32 LED keeps blinking slowly | Can't reach the broker. TLS port must be **8883**; the network must allow outgoing 8883. |
| Sign-in: “Email or password is incorrect” | Account not created in Firebase → Authentication → Users. |
| Records page: “Missing or insufficient permissions” | Firestore rules not published (step 1.5). |
| Pi hotspot never appears | `journalctl -u medicart-wifi-portal -b` · needs NetworkManager (Bookworm). |

Logs on the Pi: `journalctl -u medicart-camera -f` · `journalctl -u medicart-wifi-portal -f` · `journalctl -u medicart-vision -f`
Stop the kiosk for maintenance: `pkill -f start-kiosk.sh; pkill chromium`

## 8 · Security checklist

- [ ] Sign-up disabled in Firebase Authentication (step 1.3)
- [ ] Database + Firestore rules published (no more “test mode”)
- [ ] ESP32 admin password changed from `medicart-admin`
- [ ] Pi admin password and hotspot password set during install
- [ ] MQTT topic prefix kept private (public broker: the prefix is the only lock)
- [ ] Cart account password is strong; the cart screen is the only place it is typed

Video and audio go directly between the doctor's browser and the cart (WebRTC, DTLS-SRTP encrypted). Firebase only carries the call setup messages, presence, readings and notes. Vital-sign bands shown in the UI are indicative display aids, not a diagnosis.

## 9 · How the pieces talk

| Data | Path |
|---|---|
| Live readings | ESP32 → MQTT `<prefix>/01/sensors` (1/s) → console + cart screen |
| Sensor online/offline | ESP32 retained status + last-will on `<prefix>/01/status` |
| Reading history | cart screen → Firestore `sensor_history` (every 5 s while a finger is on the sensor) |
| Latest reading | cart screen → Realtime DB `/medicart/live` |
| Cart online / in call | cart screen → Realtime DB `/carts/medicart-01` (heartbeat 30 s, auto-offline on disconnect) |
| Call setup | Realtime DB `/calls/medicart-01` — offer, answer, ICE candidates, status, doctor prompts |
| Consultation records | console → Firestore `consultations` |

**Second cart:** deploy a copy of the site with `CART_ID = 'medicart-02'` and `MQTT.topics` pointing at `<prefix>/02/...`, and build the ESP32 firmware with `TOPIC_SENSORS` / `TOPIC_STATUS` set to `<prefix>/02/...`.
