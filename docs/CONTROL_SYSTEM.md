# MediCart 3.4 — driving control

> Part of the [MediCart docs](README.md). Protocol details: [`PROTOCOLS.md`](PROTOCOLS.md).

## How it works

```
 Pi dashboard  http://medicart.local:5000  ─┐   (Rifat's web_dashboard.py, live camera with detections)
 Vercel console → Drive page  (Firebase)  ──┼─►  medicart.py on the Pi  ──USB──►  Arduino Mega (firmware 3.4)
 SSH:  medicart-drive                     ──┘    YOLO person detection          line following (IR array)
                                                 + one set of driving rules     BTS motor drivers
```

There is **one brain**: `medicart.py`. Rifat's `WebDashboard` inside it decides, every camera frame, what the Arduino gets:

| Mode | Arduino receives | What happens |
|---|---|---|
| **stopped** (start-up, emergency stop) | `STOP` | wheels still |
| **auto** | `MODE:AUTO` while the path is clear, `STOP` for a person | the Arduino follows the line by itself with the LFR code that ran on the robot (base speed 50); the camera is only the brake |
| **manual** | `MODE:MANUAL` + `DRIVE:<left>,<right>` every 0.1 s while someone holds a key, `STOP` for a person | an operator drives (starts at 40, max 180) |

Safety:
- The Arduino stops by itself **750 ms** after the last command. That covers a crash of `medicart.py`, the Pi, or a USB cable.
- In Manual, the operator's command lasts **0.5 s** (Pi dashboard, SSH) or **0.7 s** (Vercel console). Let go and the wheels stop.
- A person in the stop zone, or the camera / YOLO not running, means `STOP`, in **both** Auto and Manual. That's Rifat's rule.
- **Emergency stop** latches. Only an explicit Auto or Manual releases it.
- A page opened before an emergency stop can't undo it: Rifat's stop token.
- The robot **starts stopped** after every restart. Someone must choose Auto or Manual.

## Files

| File | What it is |
|---|---|
| `arduino/medicart/medicart.ino` | **Firmware 3.4** = the LFR code that ran on the robot (unchanged, smooth stop) + the Nov 11 reverse turning + the Pi protocol |
| `arduino/medicart_lfr_original.ino.txt` | the LFR code that ran on the robot, exactly as it was |
| `arduino/history/medicart_fw3.3.ino.txt`, `medicart_fw3.2.ino.txt`, `medicart_fw3.1.ino.txt`, `medicart_fw3.0.ino.txt` | older firmware (kept) |
| `arduino/history/medicart_rifat_final.ino.txt` | Rifat's final firmware exactly as he sent it |
| `arduino/medicart_test/medicart_test.ino` | stand-alone bench sketch for testing an IR array (no Pi) |
| `MideCart/vision/medicart.py` | the robot program; dashboard integration marked `[DASH-3]` |
| `MideCart/vision/web_dashboard.py` | Rifat's dashboard, refined (marked `[MC-3]`); `web_dashboard.rifat.py` is his original |
| `MideCart/vision/drive_link.py` | sends `STOP` / `MODE:AUTO` / `MODE:MANUAL` + `DRIVE` at the right rate; reads `FW` / `STAT` lines |
| `MideCart/vision/firebase_bridge.py` | connects the Vercel console's Drive page |
| `MideCart/vision/templates/dashboard.html`, `static/` | the Pi dashboard page (works without internet) |
| `raspberry_pi/control/medicart-drive` | SSH keyboard driving |
| `raspberry_pi/install.sh` | asks for the cart password and a dashboard password, installs Flask, sets up everything |

What changed in Rifat's `web_dashboard.py` (refinements, his API is unchanged):
- the Vercel console and SSH use the same rules;
- a password is needed when the page is opened from another device;
- the status also shows the firmware, line sensors, driver, "person blocks manual" and the last 10 events;
- the video is shrunk to 640 px once (less CPU);
- there's no log line per status request.

## Firmware 3.4 in the Serial Monitor (115200, any line ending)

Line following, the Serial Monitor manual mode and the **smooth stop** are **the LFR code that ran on the robot, unchanged**
(`arduino/medicart_lfr_original.ino.txt`): same pins, base speed 50, steering gain 20 (max 40), position
filter 25 ms, slew 300 PWM/s, lost-line search 1.5 s at 50, black line = LOW. Every stop ramps down at 300 PWM/s (from 50: about 0.17 s; from 180: about 0.6 s), including a Pi STOP for a person and the 750 ms timeout. A host test runs both for
240 000 ms of random line patterns and gets the same PWM on pins 2–5 every millisecond.

What is added around it: the Pi decides **when** it may line-follow (`SAFE` / `MODE:AUTO`, `STOP` for a person,
stop by itself 750 ms after the last Pi command), Pi manual driving (`MODE:MANUAL` + `DRIVE:l,r`, max 180, smooth like
the LFR code), `QR:1..10`, `STAT`, the buzzer, and a live view.

**Reverse turning (from the Nov 11 LFR code, `arduino/medicart_lfr_nov11.ino.txt`), added in 3.4:**

| Only this sensor sees the line | Left wheel | Right wheel |
|---|---|---|
| S2 (slide left) | back 15 | forward 50 |
| S1 (sharp left) | back 50 | forward 50 |
| S4 (slide right) | forward 50 | back 15 |
| S5 (sharp right) | forward 50 | back 50 |

All other patterns: the LFR code above, unchanged. The turns go through the same smooth ramp. `REVERSE_TURN_ASSIST = false`
at the top of the file gives exactly the LFR code without it.

At power-on: `MediCart ready`, `FW medicart 3.4`, then 5 times a second
```
VIEW S1-S5 00100 | AUTO STOPPED | L 0 R 0 | pos 0.00 | hb 1200ms
```
(the Pi sends `VIEW 0` to switch it off).

| Type | Does |
|---|---|
| `p` | manual mode on / off (the LFR code's key) |
| `w` `a` `s` `d` | forward / left (0, 50) / back / right (50, 0), latched, in manual mode |
| `x` or space | stop |
| `l` | line following **without the Pi** for 60 s (no camera brake); `l` again = 60 s more |
| `v` | live view on / off |
| `i` | sensors once |
| `h` | help |
| `STAT` | the status line the Pi uses |

A Pi command always ends `p` manual and the `l` run.

## Settings on the Pi: `/etc/medicart/control.env` (root only)

| Key | Meaning |
|---|---|
| `CONTROL_EMAIL`, `CONTROL_PASSWORD` | cart account, for the Vercel Drive page |
| `DASHBOARD_PASSWORD` | password for the Pi dashboard from other devices (blank = the page only works on the Pi itself) |
| `DASHBOARD_HOST` / `DASHBOARD_PORT` | `0.0.0.0` / `5000` |
| `START_MODE` | `stopped` (default) or `auto` |
| `MANUAL_SPEED` | manual speed at start-up, default `40` (the LFR runs at 38) |

After changing it, run `sudo systemctl restart medicart-vision`.

## Tests (`bash tests/run_all.sh`)

1. **Firmware 3.4:** 240 000 ms of random line patterns give exactly the same PWM on pins 2–5 as the LFR code; plus the Pi protocol and the keys.
2. **Test sketch, 30 checks.**
3. **The real `medicart.py`, 42 checks.** Its main loop runs with simulated camera frames, a person switch instead of YOLO, firmware 3.4 in real time on a virtual serial port, and a local stand-in for Firebase. Covers Auto, a person stop, Manual, let-go stop, speed, the 180 cap, a person blocking Manual, emergency stop with the stop token, the Vercel console path, SSH, the password, and a program crash while driving.
4. **Browser, 25 checks:** the Vercel console Drive page, the cart banner and the Pi dashboard page driving that rig.

All simulated. Before patient use, repeat on the robot (wheels lifted, then a clear corridor): let-go stop, person stop in Auto and Manual, emergency stop, unplugging USB while driving, and stopping distance.


## Voice announcements (Bangla) on the cart screen

The cart screen (`cart.html`, Chromium on the Pi) plays the team's recordings on the cart's speaker.
Files: `web-src/assets/sounds/*.mp3` (built into `cart.html`).

| File | Says | Plays when |
|---|---|---|
| `0001` | অনুগ্রহ করে নির্ধারিত স্থান থেকে সরে দাঁড়ান | a person blocks the cart while it is trying to drive (see below) · not again within 15 s · never during a consultation |
| `0002` | নিরবচ্ছিন্ন সেবা নিশ্চিত করতে অনুগ্রহপূর্বক নির্ধারিত স্থানটি খালি রাখুন | still blocked 8 s later, then every 20 s |
| `0004` | এখন আপনার রক্তের নমুনা সংগ্রহ করা হবে… ডান হাতের তর্জনী আঙুলটি নির্ধারিত স্থানে স্থাপন করুন | the cart has arrived: it drove, then stood still for 3 s (not stopped by a person), the pulse sensor is online, no finger on it yet, no call · once per stop |
| `0005` | নমুনা সংগ্রহ সম্পন্ন না হওয়া পর্যন্ত অনুগ্রহ করে আঙুলটি স্থির রাখুন | a finger is placed (measuring starts); the finger moves (ESP32 `motion`, at most every 12 s) |
| `0006` | আপনার সকল নমুনা সফলভাবে সংগ্রহ করা হয়েছে, সহযোগিতার জন্য ধন্যবাদ | the reading locks (once per finger) |
| `0009` | আগামী এক মিনিটের মধ্যে চিকিৎসকের সঙ্গে ভিডিও পরামর্শ সেবা শুরু হবে… ধন্যবাদ | right after `0006`, if no call is on |
| `0008` | আপনার সঙ্গে কথা বলার জন্য চিকিৎসক প্রস্তুত রয়েছেন | a doctor calls: plays first, then the ringtone; auto-answer waits until it has finished |
| `0010` | অনুগ্রহ করে আপনার জিনিসপত্র নিয়ে নিন | **only** when someone presses **Take your items** on the console's Drive page |

- Every announcement is automatic except `0010`. The console's **Drive** page has one button, **Take your items**; it shows *Played on the cart ✓* when the cart has played it (Firebase `carts/<id>/announce`).
- Cart settings (hold the gear): **Voice announcements On/Off** and **Test voice**. Off silences the automatic ones; **Take your items** still plays.
- `0010` is a computer voice made with eSpeak NG. To use a recording in the same voice as the others: put it in `web-src/assets/sounds/0010.mp3`, run `python3 tools/build_pages.py`, upload `cart.html`.
- One sound at a time; the same sound is never queued twice.
- **Quiet during a consultation:** no automatic announcement plays while a doctor's video call is on, nor for 60 s after it ends (the patient is still in front of the camera). A sound that is playing when the call is answered stops at once. Only **Take your items** (pressed on purpose) still plays.
- `0001`/`0002` only when the cart is actually trying to drive: Auto (line following) with a person in the stop zone, or Manual while an operator is pressing a drive key and a person blocks it. A person in front of a cart that is parked in Manual is not announced.
