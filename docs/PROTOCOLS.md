# MediCart protocols and configuration reference

Every message, topic, data path and setting that connects the parts of MediCart.

- [1. Pi ↔ Arduino serial](#1-pi--arduino-serial)
- [2. Arduino Serial Monitor keys](#2-arduino-serial-monitor-keys)
- [3. MQTT (ESP32 → web)](#3-mqtt-esp32--web)
- [4. ESP32 Serial Monitor](#4-esp32-serial-monitor)
- [5. Firebase Realtime Database](#5-firebase-realtime-database)
- [6. Firestore](#6-firestore)
- [7. Voice announcements](#7-voice-announcements)
- [8. Configuration files](#8-configuration-files)
- [9. Ports and hostnames](#9-ports-and-hostnames)

---

## 1. Pi ↔ Arduino serial

USB serial, **115200 baud**, one command per line (any line ending).

### Pi → Arduino

| Command | Effect |
|---|---|
| `SAFE` or `MODE:AUTO` | the path is clear: line following allowed |
| `STOP` | smooth stop (300 PWM/s ramp) and stay stopped: a person in the way, emergency stop, or stopped mode |
| `MODE:MANUAL` | manual mode, waits for `DRIVE` |
| `DRIVE:<left>,<right>` | wheel PWM −180…180 (capped at 180), ramped like the LFR code |
| `QR:<n>` (n = 1…10) | station output *n* HIGH, all others LOW |
| `RESET_QR` | all station outputs LOW |
| `STAT` | print one status line (below) |
| `VIEW 0` / `VIEW 1` | live view lines off / on (the Pi sends `VIEW 0` when it connects) |

**Watchdog:** no Pi command for **750 ms** → the motors stop by themselves. `medicart.py` sends a command every camera frame.

### Arduino → Pi

| Line | When |
|---|---|
| `MediCart ready` | boot |
| `FW medicart 3.4` | boot (the Pi reads the version from it) |
| `VIEW S1-S5 00100 \| AUTO STOPPED \| L 0 R 0 \| pos 0.00 \| hb 1200ms` | 5 times a second while the view is on |
| `STAT mode=AUTO obstacle=0 hbAge=42 motor=50,50 line=00100 lost=0 fw=3.4` | reply to `STAT` |
| `DBG obstacle=1 hbAge=…` · `DBG lineLost=1` · `DBG motor=l,r` | on change (motor at most every 500 ms), for the Pi log |

## 2. Arduino Serial Monitor keys

For bench tests without the Pi. **There is no camera brake** in `p` or `l`.

| Key | Effect |
|---|---|
| `p` | manual mode on / off (the LFR code's key) |
| `w` · `a` · `s` · `d` | forward · left (0, 50) · back · right (50, 0), latched |
| `x` or space | stop |
| `l` | line following without the Pi for 60 s (`l` again = 60 s more) |
| `v` | live view on / off |
| `i` | sensors once |
| `h` | help |

Any Pi command ends `p` manual mode and an `l` run.

## 3. MQTT (ESP32 → web)

| | ESP32 | Browsers |
|---|---|---|
| Broker | `broker.hivemq.com` | `wss://broker.hivemq.com:8884/mqtt` |
| Port | 8883 (TLS) | 8884 (secure WebSocket) |
| Login | none | none |

The topics sit under a long random **prefix**. Anyone who knows it can read or fake readings, so keep it private. It's set in two places that must match: `MQTT_TOPIC_PREFIX` in [`web-src/js/config.js`](../web-src/js/config.js) and `TOPIC_PREFIX` in [`medicart_sensor.ino`](../esp32_sensor/medicart_sensor/medicart_sensor.ino).

| Topic | Retained | Payload |
|---|:-:|---|
| `<prefix>/01/sensors` | no | a reading every second (below) |
| `<prefix>/01/status` | yes | `{"device", "status": "online", "fw", "ip", "rssi", "ssid", "sensor"}`; `{"device", "status": "offline"}` is the last will |

```json
{
  "heartRate": 78,
  "spo2": 97,
  "ir": 52185,
  "red": 41000,
  "fingerDetected": true,
  "state": "locked",
  "samplesCollected": 4,
  "motion": false,
  "spo2Held": false,
  "deviceId": "medicart-sensor-01",
  "uptime": 912,
  "rssi": -58
}
```

| Field | Meaning |
|---|---|
| `heartRate` | BPM, 7-beat median, beats that don't fit the rhythm rejected. 0 = none yet |
| `spo2` | %, 5-reading median of the learned SpO₂ byte (70–100). 0 = none yet |
| `ir` | pulse wave value (PPG) |
| `red` | pulse strength |
| `fingerDetected` | a finger is on the sensor (removed only after 0.4 s out of range) |
| `state` | `waiting` · `no_finger` · `measuring` · `locked` (4 steady seconds) · `sensor_error` |
| `samplesCollected` | 0–4, shown as *Measuring n of 4* |
| `motion` | the finger is moving; beats and SpO₂ are ignored meanwhile |
| `spo2Held` | the last good SpO₂ is being held (up to 10 s) |

**Check it with MQTTX:** host `mqtts://broker.hivemq.com`, port 8883, TLS on, no login → subscribe `<prefix>/#`.

**A second cart:** `CART_ID = 'medicart-02'` and topics `<prefix>/02/...` in `config.js`, the same in the ESP32 sketch.

## 4. ESP32 Serial Monitor

115200 baud. A live line every 0.5 s, plus:

| Command | Effect |
|---|---|
| `INFO` | status and the learned SpO₂ byte |
| `RAW` | show / hide the module's SpO₂ packets in hex |
| `SPO2IDX n` | force the SpO₂ byte position (1–11); `SPO2IDX 0` = learn again |

## 5. Firebase Realtime Database

All paths need a signed-in user ([`firebase/database.rules.json`](../firebase/database.rules.json)).

| Path | Writer | Content |
|---|---|---|
| `carts/<id>` | cart screen | presence: `online`, `lastSeen`, `camera`, `mic`, `inCall`, `callDoctor`, `version` (heartbeat 30 s, `onDisconnect` → offline) |
| `carts/<id>/control/state` | Pi (`firebase_bridge.py`), once a second | mode, safety, firmware, line sensors, applied motor values, `updatedAt` |
| `carts/<id>/control/request` | console / SSH | `{mode: AUTO\|MANUAL\|STOP, by, name, id, at}` |
| `carts/<id>/control/drive` | console, every 150 ms while a key is held | `{l, r, seq, by, name, at}` |
| `carts/<id>/announce` | console → cart | `{id, sound, at, by, name}`; the cart writes back `{playedId, playedAt}` |
| `calls/<id>` | console ⇄ cart | offer, answer, `offerCandidates`, `answerCandidates` (indexed by `callId`), status, prompts |
| `medicart/live` | cart screen | the latest reading |

The cart ignores announcement requests older than 15 s and ones it has already played. The Pi ignores a request it saw at start-up, so an old request is never replayed.

## 6. Firestore

Rules: [`firebase/firestore.rules`](../firebase/firestore.rules).

| Collection | Writer | Content |
|---|---|---|
| `sensor_history` | cart screen | a reading every 5 s while a finger is on the sensor |
| `consultations` | console | started, clinician, duration, outcome, HR, SpO₂, notes |

## 7. Voice announcements

Files in [`web-src/assets/sounds/`](../web-src/assets/sounds/), built into `cart.html`. One at a time, never queued twice.

| File | Trigger |
|---|---|
| `0001` | a person blocks the cart **while it's trying to drive** (Auto + vision blocked, or Manual + a key held + blocked); 15 s cooldown |
| `0002` | still blocked 8 s later, then every 20 s |
| `0004` | arrival: drove, then still for 3 s, not blocked, sensor online, no finger, no call; once per stop |
| `0005` | finger placed (measuring starts); finger moving (at most every 12 s) |
| `0006` | reading locked (once per finger) |
| `0009` | right after `0006` if no call or ringing |
| `0008` | a doctor calls: plays, then the ringtone; auto-answer waits for it (max 20 s) |
| `0010` | **only** from *Take your items* on the Drive page |

**Quiet rule:** no automatic announcement while a call is on, nor for 60 s after it ends. A sound playing when the call is answered stops at once. Only `0010` (pressed on purpose) still plays.

## 8. Configuration files

### Website: `web-src/js/config.js`

| Key | Meaning |
|---|---|
| `APP_VERSION` | shown on the System page |
| `CART_ID`, `CART_LABEL` | which cart this deployment talks to |
| `FIREBASE_CONFIG` | Firebase web config (public by design) |
| `MQTT_TOPIC_PREFIX`, `MQTT` | broker URL, optional login, topics |
| `ICE_SERVERS` | STUN + TURN for WebRTC |
| `CART_VIDEO`, `DOCTOR_VIDEO` | camera sizes |
| `CART_CAMERA_HINT` | `MediCart-Call`: the virtual camera the cart screen picks |

After editing: `python3 tools/build_pages.py`, then push `index.html` and `cart.html`.

### Raspberry Pi: `/etc/medicart/`

| File | Keys |
|---|---|
| `control.env` (root-only) | `CART_ID`, `FIREBASE_API_KEY`, `FIREBASE_DB_URL`, `CONTROL_EMAIL`, `CONTROL_PASSWORD` (cart account), `DASHBOARD_HOST` (`0.0.0.0`), `DASHBOARD_PORT` (`5000`), `DASHBOARD_PASSWORD`, `START_MODE` (`stopped`/`auto`), `MANUAL_SPEED` (`40`) |
| `camera.conf` | `CAMERA_DEVICE` (`auto`), `CAMERA_FORMAT` (`mjpeg`), `CAMERA_SIZE` (`1280x720`), `CAMERA_FPS` (`30`) |
| `kiosk.conf` | `KIOSK_URL` (the `/cart` page) |
| `portal.conf` | `ADMIN_PASSWORD`, `AP_SSID` (`MediCart-Cart-Setup`), `AP_PASSWORD` (8+ characters), `IFACE` (`wlan0`), `BOOT_GRACE` (`45` s), `PORTAL_WINDOW` (`600` s) |
| `medicart.env` | `VISION_SCRIPT`, `VISION_PYTHON`, `MEDICART_VIDEO_SOURCE=10` |

Templates: [`raspberry_pi/config/*.example`](../raspberry_pi/config/). After a change: `sudo systemctl restart medicart-vision` (or `medicart-camera`, `medicart-wifi-portal`).

### ESP32: setup page

Wi-Fi network and password, broker host / port / login (*Advanced*), admin password. Stored in flash. Open it from the setup hotspot or at `http://medicart-sensor.local`.

## 9. Ports and hostnames

| What | Where |
|---|---|
| Doctor console | `https://medicart-iub.vercel.app/` |
| Cart screen | `https://medicart-iub.vercel.app/cart` |
| Pi dashboard | `http://medicart.local:5000` (or the Pi's IP) |
| Pi Wi-Fi setup | hotspot `MediCart-Cart-Setup` → `http://10.42.0.1` |
| ESP32 setup | hotspot `MediCart-Sensor-XXXX` (password `medicart`) → `http://192.168.4.1` · running: `http://medicart-sensor.local` |
| Virtual cameras | `/dev/video10` MediCart-Vision · `/dev/video11` MediCart-Call |
