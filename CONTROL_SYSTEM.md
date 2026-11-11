# MediCart 3.1 — driving control

## How it works

```
 Pi dashboard  http://medicart.local:5000  ─┐   (Rifat's web_dashboard.py, live camera with detections)
 Vercel console → Drive page  (Firebase)  ──┼─►  medicart.py on the Pi  ──USB──►  Arduino Mega (firmware 3.1)
 SSH:  medicart-drive                     ──┘    YOLO person detection          line following (IR array)
                                                 + one set of driving rules     BTS motor drivers
```

There is **one brain**: `medicart.py`. Rifat's `WebDashboard` inside it decides, every camera frame, what the Arduino gets:

| Mode | Arduino receives | What happens |
|---|---|---|
| **stopped** (start-up, emergency stop) | `STOP` | wheels still |
| **auto** | `MODE:AUTO` while the path is clear, `STOP` for a person | the Arduino follows the line by itself with the original LFR code (PWM 38); the camera is only the brake |
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
| `arduino/medicart/medicart.ino` | **Firmware 3.1** = the original LFR code (same pins, PWM 38 / 15, same table, same 50 ms cycle) + the Pi protocol + Serial Monitor tools |
| `arduino/medicart_lfr_original.ino.txt` | the LFR code that ran on the robot, exactly as it was |
| `arduino/medicart_fw3.0.ino.txt` | firmware 3.0 (Rifat's line-following table) |
| `arduino/medicart_rifat_final.ino.txt` | Rifat's final firmware exactly as he sent it |
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

## Firmware 3.1 in the Serial Monitor (115200, any line ending)

At power-on: `MediCart ready`, `FW medicart 3.1`, then a live view 5 times a second while it isn't line following:
```
VIEW AUTO STOPPED hb 1200ms	S1: 0	S2: 0	S3: 1	S4: 0	S5: 0	| PWM_L: 0	PWM_R: 0
```
While it follows the line it prints exactly what the original LFR code printed, every 50 ms (`forwrd`, `slide left (active reverse brake)`, … then the `S1: … | PWM_L … PWM_R …` line). When the Pi connects it sends `VIEW 0` to switch this off.

Line following (unchanged from the original code):

| S1 S2 S3 S4 S5 | Action | Left | Right |
|---|---|---|---|
| 0 0 1 0 0 | forward | +38 | +38 |
| 0 1 0 0 0 | slide left | −15 | +38 |
| 1 0 0 0 0 | sharp left (pivot) | −38 | +38 |
| 0 0 0 1 0 | slide right | +38 | −15 |
| 0 0 0 0 1 | sharp right (pivot) | +38 | −38 |
| 1 1 1 1 1 | junction: stop | 0 | 0 |
| 0 0 0 0 0 | 1 cycle: keep forward; 2 cycles: stop | | |
| anything else | forward | +38 | +38 |

| Type | Does |
|---|---|
| `l` or `1` | line following without the Pi for 60 s (**no camera brake**); `l` again = 60 s more |
| `w` `a` `s` `d` | forward / pivot left / back / pivot right for 1 s at 38 |
| `x` | stop everything |
| `+` / `-` | key speed ±10 |
| `v` | live view on / off |
| `i` | sensors once |
| `t` | finger test (wheels lifted): cover sensors, the wheels do what the LFR would do |
| `TESTL` / `TESTR` | spin one wheel |
| `FLIPL` / `FLIPR` / `SWAP` | fix a reversed wheel / swapped sides (saved) |
| `SETTINGS` / `FACTORY` | show / clear the wheel fixes |
| `STAT` | the status line the Pi uses |

The Pi protocol is unchanged: `SAFE`, `STOP`, `MODE:AUTO`, `MODE:MANUAL`, `DRIVE:l,r`, `QR:1..10`, `RESET_QR`. A Pi command always ends a Serial Monitor test.

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

1. **Firmware 3.1:** 3,000 random sensor cycles give exactly the same PWM on pins 2–5 as the original LFR code; plus the Pi protocol and all the tools.
2. **Test sketch, 30 checks.**
3. **The real `medicart.py`, 42 checks.** Its main loop runs with simulated camera frames, a person switch instead of YOLO, firmware 3.1 in real time on a virtual serial port, and a local stand-in for Firebase. Covers Auto, a person stop, Manual, let-go stop, speed, the 180 cap, a person blocking Manual, emergency stop with the stop token, the Vercel console path, SSH, the password, and a program crash while driving.
4. **Browser, 25 checks:** the Vercel console Drive page, the cart banner and the Pi dashboard page driving that rig.

All simulated. Before patient use, repeat on the robot (wheels lifted, then a clear corridor): let-go stop, person stop in Auto and Manual, emergency stop, unplugging USB while driving, and stopping distance.
