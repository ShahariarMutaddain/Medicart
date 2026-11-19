# Changelog

MediCart has three versioned parts: the **Arduino firmware**, the **ESP32 sensor firmware** and the **web console / cart screen**. Dates are 2026.

## Web 3.5.0 · November 19
- **Bangla voice announcements** on the cart screen (`0001`–`0010`), all automatic except *Take your items*.
- *Take your items* button on the Drive page, with *Played on the cart ✓* confirmation (`carts/<id>/announce`).
- **Silent during video consultations** and for 60 s after; a playing sound stops when the call is answered.
- "Step aside" only when the cart is actually trying to drive (Auto, or Manual with a key held).
- Arrival prompt (`0004`) once per stop; `0005`/`0006`/`0009` from the sensor's state.
- Cart settings: *Voice announcements On/Off*, *Test voice*.
- `build_pages.py` inlines mp3 files; `cart.html` is self-contained (~1 MB).

## Arduino firmware 3.4 · November 18
- Based on the team's **Nov 18 LFR code**, unchanged: proportional steering, 25 ms filter, **smooth stop** (300 PWM/s).
- Adds the **reverse-turn assist** from the Nov 11 code (S2 → −15/50, S1 → −50/50, mirrored), switchable with `REVERSE_TURN_ASSIST`.
- Host simulation proves identical PWM to the LFR code.

## Arduino firmware 3.3 · November 18
- Nov 18 LFR code with its smooth stop for every stop (person, timeout, `x`).

## Arduino firmware 3.2 · November 16
- Integrated the Nov 16 LFR code, the version that ran smoothly on the robot.

## ESP32 sensor 3.0.0 · November
- New **UART pulse-oximeter** module (GPIO 16/17) replaces the MAX30105.
- Motion detection, SpO₂ hold (10 s), 7-beat HR median, SpO₂ byte learning saved in flash.
- Sensor engine in its own FreeRTOS task; automatic detection of swapped wires and baud rate.
- New MQTT fields `motion` and `spo2Held`.

## Arduino firmware 3.1 · November 11
- Integrated the Nov 11 LFR code (pins, PWM, speeds) into the Pi-controlled architecture.

## Raspberry Pi · November
- Ubuntu 24.04 on the Raspberry Pi 5: **plug-and-play autostart** (`ubuntu-autostart.sh`), `medicart-status`.
- Firebase bridge with sign-in backoff.

## Arduino firmware 3.0 · November 9 · "MediCart 3"
- Pi-controlled driving: `MODE:AUTO`, `MODE:MANUAL`, `DRIVE:l,r`, 750 ms watchdog, `STAT`, live view.
- Three remotes with one set of rules: Vercel Drive page, Pi dashboard, SSH.

## MediCart 2.0 · October–November
- Doctor console and cart screen on Vercel + Firebase, WebRTC video calls with TURN, HiveMQ public broker, one camera split for YOLO and calls, Wi-Fi setup portals.

## MideCart-of-IUB · prototype
- Rifat Ahmed's original robot: YOLO person detection with distance, ArUco/QR stations, Arduino line following, Pi dashboard. Kept in `MideCart/` and `arduino/history/`.
