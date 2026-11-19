<div align="center">

<img src="docs/images/banner.jpg" alt="MediCart: the autonomous medical cart of the IUB Robotics Team" width="100%">

<br>

**An autonomous hospital delivery cart that follows a line to the patient, stops for people, measures heart rate and SpO₂,<br>and lets a doctor examine the patient over a live video call, in English and Bangla.**

<br>

[![Live console](https://img.shields.io/badge/live-medicart--iub.vercel.app-0A2647?style=for-the-badge&logo=vercel&logoColor=white)](https://medicart-iub.vercel.app)
[![Tests](https://img.shields.io/github/actions/workflow/status/tahsanmasum/MediCart/tests.yml?branch=main&style=for-the-badge&label=tests&logo=githubactions&logoColor=white)](../../actions/workflows/tests.yml)
[![License](https://img.shields.io/badge/license-MIT-C8861B?style=for-the-badge)](LICENSE)

![Arduino firmware](https://img.shields.io/badge/Arduino_Mega-firmware_3.4-00878F?logo=arduino&logoColor=white)
![ESP32](https://img.shields.io/badge/ESP32_sensor-3.0.0-E7352C?logo=espressif&logoColor=white)
![Raspberry Pi 5](https://img.shields.io/badge/Raspberry_Pi_5-Ubuntu_24.04-A22846?logo=raspberrypi&logoColor=white)
![YOLOv8](https://img.shields.io/badge/vision-YOLOv8n--pose-111F68?logo=opencv&logoColor=white)
![Web](https://img.shields.io/badge/web_console-3.5.0-0A2647?logo=javascript&logoColor=white)
![Firebase](https://img.shields.io/badge/Firebase-Auth_%C2%B7_RTDB_%C2%B7_Firestore-FFCA28?logo=firebase&logoColor=black)
![MQTT](https://img.shields.io/badge/MQTT-HiveMQ_TLS-660066?logo=mqtt&logoColor=white)
![WebRTC](https://img.shields.io/badge/WebRTC-video_calls-333333?logo=webrtc&logoColor=white)

<br>

[**What it is**](#-what-is-medicart) ·
[**See it**](#-see-it) ·
[**Features**](#-features) ·
[**How it works**](#-how-it-works) ·
[**Hardware**](#-hardware) ·
[**Quick start**](#-quick-start) ·
[**Using it**](#-using-medicart) ·
[**Safety**](#-safety-by-design) ·
[**Reference**](#-reference) ·
[**Tests**](#-tests) ·
[**Team**](#-team)

</div>

---

## 🩺 What is MediCart?

MediCart is a medical cart built by the **IUB Robotics Team** at the Department of Electrical & Electronic Engineering, **Independent University, Bangladesh**. It carries medicine and a pulse sensor to a patient's bed, so a doctor doesn't have to be in the room to see them.

| | |
|---|---|
| **The problem** | In a busy ward, a doctor can't be at every bed. Nurses spend time walking medicine around, and patients wait. |
| **What MediCart does** | It drives itself along a black line on the floor to a numbered station (bed). It brakes for anyone who steps in front of it. At the bed, it asks the patient in Bangla to place a finger on the sensor, measures heart rate and blood oxygen, and connects them to a doctor on a video call. |
| **Who uses it** | **Doctors** use the web console from any laptop or phone. **Patients** see the cart's screen and hear it speak in Bangla. **Nurses and the team** drive it, test it, and set it up. |
| **What it's made of** | An **Arduino Mega** drives the wheels and follows the line. A **Raspberry Pi 5** with one USB camera is the brain: it runs YOLO person detection, the cart screen and the video call. An **ESP32** reads the finger pulse oximeter and sends the readings over Wi-Fi. A **web console** hosted on Vercel ties it together through Firebase and MQTT. |

> [!NOTE]
> MediCart is a university research and demonstration project. It is **not a certified medical device**. Vital-sign bands shown on screen are display aids, not a diagnosis.

---

## 📸 See it

<table>
  <tr>
    <td width="33%"><img src="docs/images/photos/cart-front.jpg" alt="MediCart from the front, standing on its guide line"></td>
    <td width="33%"><img src="docs/images/photos/cart-three-quarter.jpg" alt="MediCart three-quarter view with the medicine drawer"></td>
    <td width="33%"><img src="docs/images/photos/cart-drawers.jpg" alt="Medicine drawers 2 and 3"></td>
  </tr>
  <tr>
    <td align="center"><sub><b>On the line.</b> Monitor and webcam on top, electronics in the bottom tray.</sub></td>
    <td align="center"><sub><b>Medicine drawer 1</b> (ওষুধ প্রদান: "provide medicine").</sub></td>
    <td align="center"><sub><b>Drawers 2 and 3</b> on the other side.</sub></td>
  </tr>
</table>

<table>
  <tr>
    <td width="50%"><img src="docs/images/screens/console-overview.png" alt="Doctor console overview with live heart rate and SpO2"></td>
    <td width="50%"><img src="docs/images/screens/cart-vitals.png" alt="Cart screen showing a completed reading in English and Bangla"></td>
  </tr>
  <tr>
    <td align="center"><sub><b>Doctor console</b>: the care loop, live vitals and the care team.</sub></td>
    <td align="center"><sub><b>Cart screen</b>: what the patient sees, in English and Bangla.</sub></td>
  </tr>
</table>

<details>
<summary><b>🖥️ More screenshots: every console page, the cart screen and phone views</b></summary>
<br>

| Sign in | Vitals and trends |
|---|---|
| ![Sign-in page](docs/images/screens/console-signin.png) | ![Vitals page with trend charts](docs/images/screens/console-vitals.png) |
| **Consult** (video call) | **Drive** (Auto / Manual / E-stop / Take your items) |
| ![Consult page](docs/images/screens/console-consult.png) | ![Drive page](docs/images/screens/console-drive.png) |
| **Records** | **Data stream** |
| ![Records page](docs/images/screens/console-records.png) | ![Data stream page](docs/images/screens/console-stream.png) |
| **System** | **Cart screen, waiting for a doctor** |
| ![System page](docs/images/screens/console-system.png) | ![Cart screen idle](docs/images/screens/cart-idle.png) |

| Phone: Overview | Phone: Drive |
|---|---|
| <img src="docs/images/screens/phone-overview.png" width="300" alt="Console overview on a phone"> | <img src="docs/images/screens/phone-drive.png" width="300" alt="Drive page on a phone"> |

<sub>Screenshots are taken automatically from the real pages, driven by the test rig (simulated robot and sensor). See <a href="tests/">tests/</a>.</sub>
</details>

<details>
<summary><b>🔧 Inside the cart: the electronics</b></summary>
<br>

| | | |
|---|---|---|
| ![Arduino Mega and two BTS7960 drivers](docs/images/photos/mega-and-bts7960.jpg) | ![Raspberry Pi 5 in its case](docs/images/photos/raspberry-pi5-case.jpg) | ![5-channel IR line sensor](docs/images/photos/ir-array-5ch.jpg) |
| **Arduino Mega 2560** + two **BTS7960** motor drivers | **Raspberry Pi 5**, the brain | **5-channel IR array** (OUT1–OUT5 → S1–S5) |
| ![IR array mounted over the black line](docs/images/photos/ir-array-mounted.jpg) | ![Drive motor and wheel](docs/images/photos/drive-motor-wheel.jpg) | ![Drive motor from the side](docs/images/photos/drive-motor-side.jpg) |
| IR array mounted **over the line**, under the front | Geared DC **drive motor** and wheel | Motor bracket under the chassis |

</details>

---

## ✨ Features

<table>
<tr>
<td width="50%" valign="top">

### 🤖 Robot
- **Line following** with the team's tuned LFR code: proportional steering, 25 ms position filter, **smooth stop** (300 PWM/s ramp), reverse-wheel assist on sharp turns.
- **Person safety brake**: YOLOv8n-pose measures how far away people are. **STOP at ≤ 1 m**, warning at ≤ 2 m, in both Auto and Manual.
- **Three ways to drive manually**: the web console, the Pi's own dashboard, or SSH.
- **Fail-safe**: the Arduino stops by itself **750 ms** after the last command from the Pi.
- **Station outputs**: ArUco/QR markers 1–10 switch Mega pins for each bed.
- **Buzzer**: solid tone when stopped for safety, short beeps when the line is lost.

</td>
<td width="50%" valign="top">

### 🩻 Telemedicine
- **Doctor console** (Vercel): sign-in, live heart rate and SpO₂, 1–30 min trends, records, system health.
- **Video consultation** over WebRTC, with TURN relay for campus Wi-Fi ↔ mobile data. The cart **answers by itself** after 3 s.
- **Ask the patient**: one tap shows a prompt on the cart in English and Bangla.
- **Consultation records** with duration, vitals and notes, plus sensor history, exportable as CSV / JSON (Firestore).
- **Pulse oximeter** on an ESP32: motion detection, held SpO₂ while the finger moves, 4-step "measuring" progress.
- **Demo data** mode to try the console without hardware.

</td>
</tr>
<tr>
<td valign="top">

### 🗣️ Bangla voice
- **8 Bangla announcements** (7 recorded by the team), played on the cart's speaker.
- **All automatic**: step aside, keep the area clear, place your finger, keep it still, reading done, the doctor is ready.
- **One button**: *Take your items* on the Drive page.
- **Silent during a consultation** and for 60 s after it.

</td>
<td valign="top">

### 🔌 Plug and play
- Power on the Pi and **everything starts**: auto-login, full-screen cart page, camera split, robot program, web link.
- **One camera, two users**: ffmpeg splits it into `/dev/video10` (YOLO) and `/dev/video11` (video calls).
- **Wi-Fi from a phone**: both the Pi and the ESP32 open a setup hotspot when they can't find a known network.
- `medicart-status` shows what's connected at a glance.

</td>
</tr>
</table>

---

## 🧭 How it works

<p align="center"><img src="docs/images/architecture.png" alt="Architecture: the doctor console, Vercel, Firebase and HiveMQ in the cloud, and inside MediCart the Raspberry Pi 5 (webcam splitter, medicart.py, cart screen, Firebase bridge, Pi dashboard), the Arduino Mega with the IR array, BTS7960 drivers and buzzer, and the ESP32 oximeter" width="100%"></p>

**In one sentence:** the doctor's browser and the cart screen talk through **Firebase**, receive the patient's readings from the **ESP32** over **MQTT**, and see and hear each other over **WebRTC**. On the cart, **`medicart.py`** watches the camera and tells the **Arduino** when it may drive.

<details>
<summary><b>🎬 A delivery, step by step (sequence diagram)</b></summary>
<br>

```mermaid
sequenceDiagram
    autonumber
    actor Op as Operator
    participant Con as Doctor console
    participant Pi as Pi (medicart.py)
    participant Mega as Arduino Mega
    participant Scr as Cart screen
    participant ESP as ESP32 sensor
    actor Pt as Patient
    actor Dr as Doctor

    Op->>Con: Drive → Auto
    Con->>Pi: Firebase carts/01/control
    loop every camera frame
        Pi->>Mega: MODE:AUTO (path clear) or STOP (person ≤ 1 m)
    end
    Mega->>Mega: follows the line (LFR code)
    Note over Scr: someone blocks the path
    Scr-->>Pt: 🔊 0001 "please step aside"
    Pi->>Mega: QR:3 (station 3 confirmed)
    Note over Scr: cart moved, now still 3 s
    Scr-->>Pt: 🔊 0004 "place your finger"
    Pt->>ESP: finger on the sensor
    ESP->>Scr: MQTT readings (measuring 1…4)
    Scr-->>Pt: 🔊 0005 "keep your finger still"
    ESP->>Con: heart rate + SpO₂ (locked)
    Scr-->>Pt: 🔊 0006 + 0009 "done, a doctor will call"
    Dr->>Con: Consult → Start video consultation
    Con->>Scr: call offer (Firebase calls/01)
    Scr-->>Pt: 🔊 0008 "the doctor is ready", then ringtone
    Scr->>Con: answers by itself after 3 s
    Con->>Scr: WebRTC video + audio both ways (voice announcements muted)
    Dr->>Con: notes → End call (saved to Records)
    Op->>Con: Drive → Take your items
    Scr-->>Pt: 🔊 0010 "please take your items"
```

</details>

<details>
<summary><b>📷 One camera, two users</b></summary>
<br>

The Pi has one USB webcam. YOLO needs it all the time, and so does the video call. `medicart-camera` (ffmpeg) is the only program that opens the real camera, and it copies every frame into two virtual cameras (v4l2loopback):

```mermaid
flowchart LR
    C["USB camera<br/>/dev/video0<br/>1280×720 MJPEG"] --> F["medicart-camera<br/>ffmpeg"]
    F --> V10["/dev/video10<br/>MediCart-Vision"] --> Y["medicart.py<br/>OpenCV + YOLO"]
    F --> V11["/dev/video11<br/>MediCart-Call"] --> K["cart screen<br/>video call"]
```

So obstacle detection keeps running **during** a call. `medicart.py` reads camera **10** (`MEDICART_VIDEO_SOURCE=10`, set by the installer).

</details>

---

## 🗂️ Repository map

```text
MediCart/
├── index.html · cart.html · vercel.json   ← the website Vercel serves (built, self-contained)
├── web-src/            source of the website: console + cart screen (HTML, CSS, JS, Bangla sounds)
├── tools/              build_pages.py: inlines web-src/ into index.html and cart.html
├── arduino/            Arduino Mega firmware 3.4, the team's LFR codes, a bench test sketch, history
├── MideCart/           Rifat's original robot program (medicart.py, YOLO, Pi dashboard) + Firebase bridge
├── raspberry_pi/       installer, plug-and-play autostart, camera splitter, kiosk, Wi-Fi portal, systemd
├── esp32_sensor/       ESP32 pulse-oximeter firmware 3.0.0 + ready-to-flash image
├── firebase/           Realtime Database and Firestore security rules
├── tests/              firmware simulations, end-to-end robot tests, browser tests (no hardware needed)
├── docs/               setup guide, control system, hardware reference PDF, protocols, troubleshooting
└── .github/            CI workflow, issue and pull-request templates
```

| I want to… | Go to |
|---|---|
| Set the whole thing up from zero | [`docs/SETUP_GUIDE.md`](docs/SETUP_GUIDE.md) and [`docs/DEPLOYMENT.md`](docs/DEPLOYMENT.md) |
| Understand how driving and safety work | [`docs/CONTROL_SYSTEM.md`](docs/CONTROL_SYSTEM.md) |
| Rebuild or rewire the body | [`docs/HARDWARE.md`](docs/HARDWARE.md) and [`docs/MediCart_Hardware_Reference.pdf`](docs/MediCart_Hardware_Reference.pdf) |
| Look up a serial command, MQTT field or Firebase path | [`docs/PROTOCOLS.md`](docs/PROTOCOLS.md) |
| Fix something that isn't working | [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) |
| See how the parts fit together | [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) |
| Change the website | [`web-src/README.md`](web-src/README.md) |
| Flash the Arduino or the ESP32 | [`arduino/README.md`](arduino/README.md) · [`esp32_sensor/README.md`](esp32_sensor/README.md) |
| Install the Pi | [`raspberry_pi/README.md`](raspberry_pi/README.md) |
| Run the tests | [`tests/README.md`](tests/README.md) |

---

## 🧰 Tech stack

| Layer | Hardware | Software |
|---|---|---|
| **Motion** | Arduino Mega 2560 · 2 × BTS7960 (IBT-2) · 2 geared DC motors · 5-ch digital IR array · active buzzer | C++ (Arduino) firmware 3.4 |
| **Brain** | Raspberry Pi 5 (5 V 5 A) · USB webcam · speaker · microphone · HDMI monitor | Ubuntu 24.04 · Python 3 · Ultralytics YOLOv8n-pose · OpenCV · Flask · pyserial · ffmpeg · v4l2loopback · systemd · Chromium kiosk |
| **Vitals** | ESP32 Dev Module (WROOM) · UART pulse-oximeter module | C++ (Arduino-ESP32) firmware 3.0.0 · FreeRTOS task · PubSubClient · captive portal |
| **Cloud** | n/a | Vercel (static hosting) · Firebase Auth, Realtime Database, Firestore · HiveMQ public MQTT broker (TLS 8883 / WSS 8884) · Metered TURN |
| **Web** | Any browser | Vanilla JS (ES modules) · Firebase JS SDK 10 · MQTT.js · Chart.js · WebRTC · HTMLAudioElement with `setSinkId` |
| **Tests** | none needed | g++ host simulation · Python e2e rig with a virtual serial port · Playwright + Chromium · GitHub Actions |

---

## 🔩 Hardware

<details open>
<summary><b>Bill of materials</b></summary>
<br>

| # | Part | Qty | Role |
|---|---|:-:|---|
| 1 | Raspberry Pi 5 + active cooler + **5 V 5 A** USB-C supply | 1 | Brain: vision, cart screen, calls, web link |
| 2 | Arduino Mega 2560 + USB-A → USB-B cable | 1 | Motors, line following, buzzer, station outputs |
| 3 | BTS7960 (IBT-2) motor driver | 2 | One per drive wheel |
| 4 | Geared DC motor + wheel | 2 | Differential drive: the cart turns by running the wheels at different speeds |
| 5 | 5-channel digital IR line sensor | 1 | Follows the black line, S1 on the left |
| 6 | USB webcam (with mic) | 1 | Safety brake, station markers, video call |
| 7 | Speaker, HDMI monitor | 1 each | Bangla voice and call audio, cart screen |
| 8 | Active buzzer | 1 | Safety tone, line-lost beeps |
| 9 | ESP32 Dev Module (WROOM) + UART pulse oximeter | 1 | Heart rate and SpO₂ over Wi-Fi |
| 10 | Motor battery, DC-DC converters, fuse, main switch | – | Power (see the hardware reference) |

</details>

<details>
<summary><b>📌 Arduino Mega pin map</b></summary>
<br>

| Pin | Connected to | Notes |
|---|---|---|
| **D2** | Left BTS7960, forward PWM | PWM 0–255 |
| **D3** | Left BTS7960, reverse PWM | |
| **D4** | Right BTS7960, forward PWM | |
| **D5** | Right BTS7960, reverse PWM | |
| **A8 – A12** | IR sensors **S1 – S5** | Digital; **LOW = black line** |
| **D25** | Active buzzer + | Solid = safety stop · beeps = line lost |
| **D6 – D13** | Station outputs 1 – 8 | HIGH for the confirmed station (`QR:n`) |
| **D22, D23** | Station outputs 9, 10 | |
| **USB-B** | Raspberry Pi | Serial 115200; keep **D0/D1 free** |
| R_EN, L_EN | **5 V** on both BTS7960 | Never driven by firmware; floating = wheels don't turn |

</details>

<details>
<summary><b>📌 ESP32 sensor wiring</b></summary>
<br>

| ESP32 | Oximeter module | Notes |
|---|---|---|
| **GPIO 16** (RX2) | TX | 115200 baud, packets `0x41 0x42` + 12 bytes |
| **GPIO 17** (TX2) | RX | Swapped wires or another baud rate? The firmware finds the module by itself. |
| 3V3 (or 5 V) | VCC | Whatever the module needs |
| GND | GND | A common ground is required |
| GPIO 0 (BOOT) | n/a | Hold 5 s while running → Wi-Fi setup portal |
| GPIO 2 (LED) | n/a | fast blink = setup · slow = connecting · solid = online |

Board: **ESP32 Dev Module** (classic ESP32, WROOM module). Not ESP32-S3, and not WROVER (GPIO 16/17 are used by PSRAM).

</details>

<details>
<summary><b>📌 Raspberry Pi 5 connections</b></summary>
<br>

Everything is USB or HDMI; the 40-pin header is unused.

| Port | Device |
|---|---|
| USB-C power | **5 V 5 A**. With a weaker supply the Pi limits USB to 600 mA, and the Mega and webcam drop off. |
| HDMI 0 (next to power) | Cart monitor |
| Any USB | Arduino Mega, webcam, speaker (names are found automatically) |
| Wi-Fi | Firebase, video calls, Vercel Drive page. Falls back to the `MediCart-Cart-Setup` hotspot. |

</details>

The full rebuild guide, with drawings, the power plan and a wiring log for the hardware team, is in [`docs/MediCart_Hardware_Reference.pdf`](docs/MediCart_Hardware_Reference.pdf) and [`docs/HARDWARE.md`](docs/HARDWARE.md).

---

## 🚀 Quick start

Setting up a new cart takes five stages. Each one has a short version below and a complete step-by-step guide in [`docs/`](docs/).

```mermaid
flowchart LR
    A["1 · Firebase<br/>accounts + rules"] --> B["2 · Website<br/>GitHub → Vercel"] --> C["3 · Arduino<br/>upload firmware 3.4"] --> D["4 · Raspberry Pi<br/>install.sh"] --> E["5 · ESP32<br/>flash + Wi-Fi"]
```

<details>
<summary><b>1 · Firebase</b> (≈10 min)</summary>
<br>

1. Firebase console → your project → **Authentication → Email/Password → Enable**.
2. **Users → Add user**: one per doctor, plus one **cart account** (e.g. `cart01@medicart.iub`).
3. **Settings → User actions → untick "Enable create (sign-up)"**, so strangers can't make accounts.
4. **Realtime Database → Rules** ← paste [`firebase/database.rules.json`](firebase/database.rules.json) → Publish.
5. **Firestore → Rules** ← paste [`firebase/firestore.rules`](firebase/firestore.rules) → Publish.

Using your own Firebase project? Put its web config in [`web-src/js/config.js`](web-src/js/config.js), then run `python3 tools/build_pages.py`.

</details>

<details>
<summary><b>2 · Website</b> (≈5 min)</summary>
<br>

This repository **is** the Vercel project: `vercel.json`, `index.html` and `cart.html` sit at the root, and [`.vercelignore`](.vercelignore) keeps everything else off the web.

1. [vercel.com/new](https://vercel.com/new) → import this repo → **Framework: Other**, root `./` → **Deploy**.
2. Doctor console: `https://<project>.vercel.app/` · cart screen: `https://<project>.vercel.app/cart`.

After editing `web-src/`, rebuild the two pages and push:

```bash
python3 tools/build_pages.py     # writes index.html and cart.html
git commit -am "web: …" && git push   # Vercel redeploys in about a minute
```

</details>

<details>
<summary><b>3 · Arduino Mega</b> (≈5 min)</summary>
<br>

1. Arduino IDE → open [`arduino/medicart/medicart.ino`](arduino/medicart/medicart.ino).
2. **Board: Arduino Mega or Mega 2560** → Port → **Upload**.
3. Serial Monitor at **115200**: `MediCart ready`, `FW medicart 3.4`, then live `VIEW S1-S5 …` lines.
4. Wheels lifted: `p` (manual on) → `w a s d` → `x` stop. Then type `i` with the line under the middle sensor: you should see `00100`.

On Linux, if the upload fails: `sudo usermod -aG dialout $USER`, then log out and back in.

</details>

<details>
<summary><b>4 · Raspberry Pi 5</b> (≈40 min, mostly downloads)</summary>
<br>

1. Flash **Ubuntu Desktop 24.04 LTS (64-bit)** with Raspberry Pi Imager.
2. Copy `MideCart/` and `raspberry_pi/` to the Pi's home folder.
3. Install the robot program's libraries:
   ```bash
   cd ~/MideCart && python3 -m venv venv && venv/bin/pip install ultralytics pyserial flask requests
   sudo usermod -aG dialout,video,audio $USER && sudo reboot
   ```
4. Install MediCart (it asks for the cart page URL, the hotspot and admin passwords, the cart account and a dashboard password):
   ```bash
   cd ~/raspberry_pi && chmod +x install.sh && sudo ./install.sh
   ```
5. Reboot. The cart screen opens full screen. Sign in once with the cart account, then hold the ⚙️ gear to choose the microphone and speaker.

Check everything at once with `medicart-status`. Full walkthrough: [`docs/SETUP_GUIDE.md`](docs/SETUP_GUIDE.md).

</details>

<details>
<summary><b>5 · ESP32 sensor</b> (≈5 min)</summary>
<br>

**From the browser (no Arduino IDE):** Chrome → [esptool-js](https://espressif.github.io/esptool-js/) → Connect → address **`0x0`** → [`esp32_sensor/prebuilt/medicart_sensor_v3.0.0_merged.bin`](esp32_sensor/prebuilt/) → Program.

**Or with the Arduino IDE:** install *esp32 by Espressif* and the library *PubSubClient* → open [`esp32_sensor/medicart_sensor/medicart_sensor.ino`](esp32_sensor/medicart_sensor/medicart_sensor.ino) → board **ESP32 Dev Module** → Upload.

**Wi-Fi:** phone → join `MediCart-Sensor-XXXX` (password `medicart`) → the setup page opens → admin password → your network → **change the admin password** under *Advanced* → Save. The LED goes solid and the console shows *Sensor streaming*.

</details>

> [!TIP]
> No hardware yet? Open the console and press **Demo data** in the top bar. It simulates the sensor and saves nothing.

---

## 🕹️ Using MediCart

<details open>
<summary><b>Doctor console pages</b></summary>
<br>

| Page | What you do there |
|---|---|
| **Overview** | Care loop (You → cart → patient), live heart rate / SpO₂ / IR signal, recent consultations, care team |
| **Vitals** | Big live numbers, sensor state, trends over 1 / 5 / 15 / 30 min, raw IR and RED |
| **Consult** | Start the video call, see the patient's vitals beside the video, send *Ask the patient* prompts, write notes |
| **Records** | Past consultations (clinician, duration, outcome, HR, SpO₂, notes) and sensor history · export CSV / JSON |
| **Drive** | Auto / Manual / **Emergency stop**, joystick + `W A S D`, speed limit, live robot status, **Take your items** |
| **Data stream** | Every MQTT message as it arrives, for engineers |
| **System** | Your profile, and the health of the cart screen, ESP32 sensor, MQTT link and Firebase, with versions and the cart's hardware summary |

</details>

<details>
<summary><b>Three ways to drive</b></summary>
<br>

All three go through the same rules in `medicart.py`: a person in the way means STOP, and letting go means STOP.

| Where | How | Needs internet |
|---|---|:-:|
| **Web console → Drive** | joystick or `W A S D` / arrows, `Space` = stop | ✅ |
| **Pi dashboard** `http://medicart.local:5000` | live camera with detection boxes + buttons (password from the installer) | ❌ same Wi-Fi |
| **SSH** `ssh <user>@medicart.local` → `medicart-drive` | `1` Auto · `2` Manual · `0` stop · hold `W A S D` | ❌ |

After every restart the robot is **STOPPED** until someone chooses Auto or Manual.

</details>

<details>
<summary><b>Arduino Serial Monitor keys (bench testing, no Pi)</b></summary>
<br>

| Key | Does |
|---|---|
| `p` | manual mode on / off |
| `w` `a` `s` `d` | forward / left / back / right (latched, in manual mode) |
| `x` or space | stop |
| `l` | line following **without the Pi** for 60 s (no camera brake: someone must stay next to the cart) |
| `v` · `i` · `h` | live view on/off · sensors once · help |
| `STAT` | the status line the Pi uses |

</details>

<details>
<summary><b>🗣️ Bangla voice announcements</b></summary>
<br>

| File | Says | Plays when |
|---|---|---|
| `0001` | অনুগ্রহ করে নির্ধারিত স্থান থেকে সরে দাঁড়ান | a person blocks the cart **while it's trying to drive** (15 s cooldown) |
| `0002` | …নির্ধারিত স্থানটি খালি রাখুন | still blocked after 8 s, then every 20 s |
| `0004` | …ডান হাতের তর্জনী আঙুলটি নির্ধারিত স্থানে স্থাপন করুন | the cart arrived: it drove, then stood still for 3 s, the sensor is online and no finger is on it yet |
| `0005` | …আঙুলটি স্থির রাখুন | a finger is placed, or it moves (at most every 12 s) |
| `0006` | আপনার সকল নমুনা সফলভাবে সংগ্রহ করা হয়েছে… | the reading locks |
| `0009` | আগামী এক মিনিটের মধ্যে চিকিৎসকের সঙ্গে ভিডিও পরামর্শ… | right after `0006`, if no call is on |
| `0008` | আপনার সঙ্গে কথা বলার জন্য চিকিৎসক প্রস্তুত রয়েছেন | a doctor calls (before the ringtone) |
| `0010` | অনুগ্রহ করে আপনার জিনিসপত্র নিয়ে নিন | **only** when someone presses **Take your items** |

No automatic announcement plays **during a video consultation** or for **60 s after it**. Cart settings (hold the ⚙️ gear) have *Voice On/Off* and *Test voice*.

</details>

---

## 🛡️ Safety by design

```mermaid
stateDiagram-v2
    direction LR
    [*] --> STOPPED: power on / restart
    STOPPED --> AUTO: operator chooses Auto
    STOPPED --> MANUAL: operator chooses Manual
    AUTO --> MANUAL
    MANUAL --> AUTO
    AUTO --> ESTOP: Emergency stop
    MANUAL --> ESTOP: Emergency stop
    STOPPED --> ESTOP: Emergency stop
    ESTOP --> AUTO: explicit Auto only
    ESTOP --> MANUAL: explicit Manual only

    state AUTO {
        direction LR
        Following --> Braked: person ≤ 1 m or camera/YOLO down
        Braked --> Following: path clear
    }
    state MANUAL {
        direction LR
        Driving --> Halted: key released (0.5–0.7 s) or person ≤ 1 m
        Halted --> Driving: key held, path clear
    }
```

| Rule | Where it lives |
|---|---|
| The wheels stop **750 ms** after the last Pi command. This covers a crash of the program or the Pi, or a loose USB cable. | Arduino firmware |
| A person within **1 m**, or the camera / YOLO not running → `STOP`, in **Auto and Manual** | `medicart.py` (Rifat's rule) |
| The cart **starts stopped** after every restart | `medicart.py`, firmware |
| **Emergency stop latches**. A page opened before the stop can't undo it (stop token). | `web_dashboard.py` |
| Manual commands expire after **0.5 s** (Pi, SSH) / **0.7 s** (web). Let go and it stops. | `drive_link.py`, console |
| Manual speed is **capped at 180 PWM**. Line following runs at 50. | firmware |
| The browser **never drives the motors directly**: it writes a *request*, and the Pi decides | `firebase_bridge.py` |
| Every stop is **ramped** (300 PWM/s), so the cart doesn't jerk or tip a load | firmware (LFR code) |

> [!WARNING]
> The Serial Monitor tests (`p`, `l`) have **no camera brake**. Lift the wheels for first tests and keep someone next to the cart. For hospital use, add a **hardware emergency-stop button** that cuts motor power (see the hardware reference).

---

## 📚 Reference

<details>
<summary><b>Pi ↔ Arduino serial protocol</b> (USB, 115200, newline-terminated)</summary>
<br>

| Pi sends | Arduino does |
|---|---|
| `SAFE` / `MODE:AUTO` | line following allowed (path clear) |
| `STOP` | smooth stop, stay stopped |
| `MODE:MANUAL` | manual mode, waits for `DRIVE` |
| `DRIVE:<left>,<right>` | wheel PWM −180…180, ramped |
| `QR:<1..10>` · `RESET_QR` | station output pin n HIGH (others LOW) · all LOW |
| `STAT` | replies with one status line ending `fw=3.4` |
| `VIEW 0` / `VIEW 1` | live `VIEW S1-S5 …` lines off / on |

The Arduino prints `MediCart ready` and `FW medicart 3.4` at boot. No command for 750 ms → motors stop.

</details>

<details>
<summary><b>MQTT topics and payload</b></summary>
<br>

Broker `broker.hivemq.com`: TLS **8883** for the ESP32, secure WebSocket **8884** `/mqtt` for browsers. Topics live under a private random prefix, set in `web-src/js/config.js` (`MQTT_TOPIC_PREFIX`) and in the ESP32 sketch (`TOPIC_PREFIX`). The two must match.

| Topic | Content |
|---|---|
| `<prefix>/01/sensors` | one reading per second |
| `<prefix>/01/status` | `online` / `offline` (retained + last will), IP, RSSI, firmware |

```json
{
  "heartRate": 78, "spo2": 97, "ir": 52185, "red": 41000,
  "fingerDetected": true, "state": "locked", "samplesCollected": 4,
  "motion": false, "spo2Held": false,
  "deviceId": "medicart-sensor-01", "uptime": 912, "rssi": -58
}
```

`state` is one of `waiting` · `no_finger` · `measuring` · `locked` · `sensor_error`.

</details>

<details>
<summary><b>Firebase data map</b></summary>
<br>

| Path | Written by | Holds |
|---|---|---|
| RTDB `carts/<id>` | cart screen | presence and heartbeat (auto-offline on disconnect) |
| RTDB `carts/<id>/control/state` · `/request` · `/drive` | Pi bridge · console | robot status · mode requests · manual drive commands |
| RTDB `carts/<id>/announce` | console → cart | `{id, sound, at, by}` → cart replies `{playedId, playedAt}` |
| RTDB `calls/<id>` | console ↔ cart | WebRTC offer/answer, ICE candidates, status, doctor prompts |
| RTDB `medicart/live` | cart screen | latest reading |
| Firestore `sensor_history` | cart screen | a reading every 5 s while a finger is on the sensor |
| Firestore `consultations` | console | call records: duration, vitals, notes |

Every path requires a signed-in user. Sign-up is disabled, so only accounts the admin creates can sign in.

</details>

<details>
<summary><b>Configuration files</b></summary>
<br>

| File | What you set |
|---|---|
| `web-src/js/config.js` | Firebase web config, cart ID, MQTT broker + topic prefix, TURN servers, camera sizes |
| `/etc/medicart/control.env` (Pi, root-only) | cart account, dashboard password, start mode, manual speed |
| `/etc/medicart/camera.conf` | camera device, MJPEG/raw, size, fps |
| `/etc/medicart/kiosk.conf` | cart page URL |
| `/etc/medicart/portal.conf` | Wi-Fi setup admin + hotspot passwords |
| ESP32 setup page | Wi-Fi, broker, admin password (stored in flash) |

Templates: [`raspberry_pi/config/*.example`](raspberry_pi/config/). Full list in [`docs/PROTOCOLS.md`](docs/PROTOCOLS.md).

</details>

---

## 🧪 Tests

Everything can be tested **without the robot**. The firmware runs on your PC, the real `medicart.py` drives simulated hardware over a virtual serial port, and the real web pages run in Chromium.

```bash
bash tests/run_all.sh
```

| Suite | What it proves | Checks |
|---|---|:-:|
| Firmware 3.4 host simulation | 240 000 ms of random line patterns give **exactly** the same PWM as the team's LFR code, every millisecond; plus the Pi protocol and keys | ✅ all |
| Bench test sketch | the IR viewer, calibration, manual keys and line following of `medicart_test` | 30 |
| Robot end-to-end | the real `medicart.py` + firmware: Auto, person stop, Manual, let-go stop, 180 cap, E-stop token, web path, SSH, crash while driving | 42 |
| Browser UI | console Drive page, cart banner, Pi dashboard, every voice trigger | 39 |
| Call + voice | the cart rings, says 0008, auto-answers, stays **silent during and after** the call | 6 |
| ESP32 engine | heart-rate median, SpO₂ byte learning, motion hold | ✅ all |

CI runs the firmware, ESP32 and robot suites on every push ([`.github/workflows/tests.yml`](.github/workflows/tests.yml)).

> [!IMPORTANT]
> The simulations don't replace a test on the robot. After any change, repeat on the real cart, wheels lifted and then in a clear corridor: let-go stop, person stop in Auto and Manual, emergency stop, unplugging USB while driving, and the stopping distance.

---

## 🛠️ Troubleshooting

<details>
<summary><b>The most common problems</b></summary>
<br>

| You see | Do |
|---|---|
| `medicart-status`: ✗ Arduino, ✗ camera | Pi power supply too weak (needs 5 V **5 A**), or the USB cable. Check `lsusb`. |
| Console Drive page: **Controller offline** | `journalctl -u medicart-vision \| grep FIREBASE`. If it says *sign-in failed*, re-run `sudo ./install.sh` with the right cart password. |
| Console: **Cart offline** | The cart screen isn't open or isn't signed in. Reload it with `pkill -f chromium`. |
| Call connects, then **Connection failed** | The networks block direct video. Check the TURN servers in `config.js`. |
| ESP32: **No data from oximeter** | TX ↔ RX crossed, common GND, module power. The Serial Monitor says what it found. |
| ESP32 upload: *"This chip is ESP32, not ESP32-S3"* | Board → **ESP32 Dev Module** |
| Mega upload: *permission denied* | `sudo usermod -aG dialout $USER` and log in again; choose **Arduino Mega** |
| Black screen after the boot logo | Use the HDMI port **next to the power socket** |
| Pasted command shows `^[[200~` | Open a new terminal, or run `bind 'set enable-bracketed-paste off'` |

More: [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md)

</details>

---

## 🗺️ Roadmap

- [x] Line following with the team's LFR code, smooth stop and reverse-turn assist (firmware 3.4)
- [x] YOLO person brake + three ways to drive manually
- [x] Doctor console, cart screen, WebRTC video calls with TURN
- [x] ESP32 oximeter 3.0.0 with motion hold and SpO₂ byte learning
- [x] Bangla voice announcements, quiet during consultations
- [x] Plug-and-play boot on Raspberry Pi 5 / Ubuntu 24.04
- [ ] **Vision-only navigation** (no line), in progress
- [ ] New body by the hardware team ([hardware reference](docs/MediCart_Hardware_Reference.pdf))
- [ ] Hardware emergency-stop button that cuts motor power
- [ ] Private MQTT broker for real patient data
- [ ] Station-to-medicine drawer automation

---

## 👥 Team

<div align="center">
<img src="docs/images/iub-logo.png" width="72" alt="IUB logo">

**IUB Robotics Team** · Department of Electrical & Electronic Engineering<br>
Independent University, Bangladesh (IUB) · with the IEEE Robotics & Automation Society
</div>

| Role | People |
|---|---|
| Supervisor | **Dr. Feroz Ahmed** |
| Team leads | Kalleyen Kumar Tirtho · Ankit Roy · Shahariar Mutaddain |
| Web console, telemedicine and MediCart 2.0 integration | Tahsan Masum Fahim |
| Firmware and original robot program | Rifat Ahmed · Saif Al Islam Hemal |
| Simulation | Shahariar Mutaddain · Omar Ahmed |
| Cable management | Ankit Roy · Sadman Sami |
| Looks and aesthetics | Ove Roy |

MediCart 2.0 builds on the original prototype (*MideCart-of-IUB*) by Rifat Ahmed. His files and behaviour are kept unchanged in [`MideCart/`](MideCart/) and [`arduino/history/`](arduino/history/).

---

## 📄 License and credits

MediCart's own code is released under the [MIT License](LICENSE). Third-party parts keep their own licenses:

- **Ultralytics YOLOv8** and the bundled `yolov8n*.pt` weights: [AGPL-3.0](https://github.com/ultralytics/ultralytics/blob/main/LICENSE)
- **Firebase JS SDK**, **MQTT.js**, **Chart.js**, **PubSubClient**: their respective open-source licenses
- The Bangla voice recordings (`0001`–`0009`) belong to the MediCart team. `0010` was made with eSpeak NG.

<div align="center">
<br>
<sub>Made with care in Dhaka · <i>"Teacheth man that which he knew not"</i></sub>
<br><br>
<a href="#-what-is-medicart">↑ back to top</a>
</div>
