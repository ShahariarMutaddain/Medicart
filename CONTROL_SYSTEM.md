# MediCart control system: Auto, Manual and Emergency stop

MediCart can now be driven three ways. Exactly one is active at a time:

| Mode | Who moves the wheels | How it stops |
|---|---|---|
| **AUTO** | Rifat's `medicart.py` + the Arduino's line following, exactly as before | `medicart.py` sends `STOP` for a person; no `SAFE` for 2 s → Arduino stops |
| **MANUAL** | A person: the web console **Drive** page, or `medicart-drive` over SSH | Let go → stops within 1 s. Person ahead → forward blocked. Link lost → Arduino stops after 0.5 s |
| **STOP** | Nobody: emergency stop, latched | Stays stopped (even after a reboot) until someone chooses Auto or Manual |

```
 Doctor console (Drive page) ──► Firebase  carts/medicart-01/control ──┐
 SSH:  medicart-drive ─────────► /run/medicart/control.sock ───────────┤
                                                                       ▼
 medicart.py (Rifat, YOLO) ──► /run/medicart/vision-tty ──►  medicart-control (Pi)  ──USB──►  Arduino Mega
     SAFE / STOP / QR:n                                     owns the serial port            firmware 2.1
                                                            AUTO: passes medicart.py through   BTS drivers
                                                            MANUAL: MAN:l,r every 100 ms       pins 2/3 L, 4/5 R
                                                            STOP: STOP every 250 ms
```

## What changed, file by file

| File | Change |
|---|---|
| `arduino/medicart/medicart.ino` | **Firmware 2.1** = Rifat's firmware + manual drive. All additions are marked `[CTRL-2.1]`. AUTO behaviour is unchanged. `arduino/medicart.cpp.original` is the untouched original. |
| `MideCart/vision/medicart.py` | 5 added lines: reads the serial port from `MEDICART_SERIAL_PORT` if set. Not set = original behaviour. `medicart.py.original` is untouched. The installer adds the same lines to your copy if they're missing (keeps a `.original` backup). |
| `raspberry_pi/control/medicart_control.py` | **New.** The drive controller service. |
| `raspberry_pi/control/medicart-drive` | **New.** Keyboard driving over SSH (works without internet). |
| `raspberry_pi/systemd/medicart-control.service.in` | **New.** Starts the controller at boot, before `medicart-vision`. |
| `raspberry_pi/config/control.env.example` | **New.** Settings: `/etc/medicart/control.env` on the Pi (root only). |
| `raspberry_pi/install.sh` | Asks 3 extra questions and installs the controller. |
| `raspberry_pi/systemd/medicart-vision.service.in`, `vision/run-vision.sh` | `medicart.py` uses the controller's virtual port. |
| `index.html` (console) | **New Drive page**: mode buttons, emergency stop, drive pad (mouse / touch / W A S D), speed limit, robot status, controller log. Side rail shows the drive mode. |
| `cart.html` (cart screen) | Banner at the bottom: **Manual driving · ম্যানুয়াল চালনা · Dr X** or **Emergency stop · জরুরি থামানো**. |
| `web-src/`, `tools/build_pages.py` | Readable source of both pages. Edit there, then `python3 tools/build_pages.py`. |
| `tests/` | Firmware simulation, end-to-end and browser tests (`bash tests/run_all.sh`). |

Firebase rules: **no change needed.** The control data lives under `carts/medicart-01/control`, which signed-in accounts can already read and write.

## Firmware 2.1 serial protocol (115200 baud, one command per line)

| Command | From | Effect |
|---|---|---|
| `SAFE` | medicart.py | (original) path clear → line following; refreshes the 2 s watchdog |
| `STOP` | medicart.py / controller | (original) stop. In MANUAL it also halts the wheels at once |
| `QR:1` … `QR:10`, `RESET_QR` | medicart.py | (original) bed output pins 6–13, 22, 23 |
| `MAN:<left>,<right>` | controller | **new** wheel PWM −255…255 (+ forward), capped at 200, ramped (0→200 in 0.2 s). Enters MANUAL. Must repeat within **0.5 s** or the wheels stop |
| `AUTO` | controller | **new** leave MANUAL: stop, then line follow on the next `SAFE` |
| `STAT` | controller | **new** prints `STAT mode=AUTO obstacle=0 hbAge=120 man=0,0 manAge=0 line=00100 lost=0` |

At boot it prints `MediCart ready` (unchanged) and `FW medicart-ctrl 2.1`.

## Safety rules (all tested)

1. **Dead-man, three layers.**
   - The console sends the drive command every 150 ms only while you hold. Release → one `0,0`.
   - The Pi zeroes it if nothing fresh arrives for 0.6 s.
   - The Arduino stops by itself 0.5 s after the last `MAN:`.
   - If the browser tab closes, Firebase writes `0,0` for it (onDisconnect). If the SSH session dies, the controller voids that client's command.
2. **Person ahead in MANUAL.** If `medicart.py` is sending `STOP`, the forward part of any command is removed. You can still turn on the spot or reverse.
3. **Vision not running in MANUAL.** Speed is limited to `MANUAL_SLOW_PWM` (90), and the console shows a warning.
4. **Emergency stop latches.** It ignores drive commands and `medicart.py`'s `SAFE`, survives a reboot, and only an explicit Auto or Manual releases it.
5. **Controller crash.** The Arduino watchdogs stop the wheels: 0.5 s in manual, 2 s in auto.
6. **Mode changes always pass through a stop.** Going back to Auto waits for a fresh `SAFE` from `medicart.py`.
7. **Leaving the Drive page releases the controls.** So does switching browser tabs or windows.

## Set up (after the normal Raspberry Pi setup)

1. **Flash firmware 2.1** with the wheels lifted. Open `arduino/medicart/medicart.ino` in Arduino IDE → Mega 2560 → Upload. The Serial Monitor shows `MediCart ready` then `FW medicart-ctrl 2.1`. Then run the watchdog test: `SAFE` several times → the wheels turn; stop typing → they stop within 2 s.
2. **Run the installer** (again, if you already ran it; that's safe):
   ```bash
   cd ~/raspberry_pi && sudo ./install.sh
   ```
   It asks three extra questions:

   | Question | Answer |
   |---|---|
   | Install the drive controller? | `y` |
   | Cart account email | `cart01@medicart.iub` |
   | Cart account password (hidden) | the cart account's password; it stays in `/etc/medicart/control.env`, root only |
   | Firebase apiKey / databaseURL | press Enter for both (the defaults are your medicart-e20e0 project) |

   It installs `python3-serial` and `python3-requests`, then:
   - starts `medicart-control`
   - points `medicart-vision` at `/run/medicart/vision-tty`
   - adds the 5 port lines to your `medicart.py` if they're missing
3. **Reboot:** `sudo reboot`.
4. **Check:**
   ```bash
   systemctl status medicart-control medicart-vision --no-pager
   medicart-drive status          # mode, Arduino, firmware, vision, line sensors
   journalctl -u medicart-control -f
   ```
5. **Deploy the website:** push the new `index.html` and `cart.html` to GitHub. Vercel redeploys.

## Driving

**From the console:** go to **Drive** (left menu, under Robot).
1. Click **Manual**. The pad turns from *Locked* to *Ready*.
2. Press and drag inside the circle, or hold **W A S D** or the arrow keys. Up is forward; dragging sideways near the middle turns on the spot.
3. Let go to stop. **Space** stops at once.
4. **Speed limit** caps how fast you can go (it's remembered in your browser).
5. **Emergency stop** works from any mode, any time. **Auto** hands the cart back to line following.

**Over SSH:** run `ssh pi@medicart.local`, then `medicart-drive`. This works with no internet.

| Key | Action |
|---|---|
| `2` | MANUAL |
| `1` | AUTO |
| `0` or `X` | EMERGENCY STOP |
| W A S D / arrows (hold) | drive |
| `+` / `-` | speed |
| SPACE | stop |
| `Q` | quit |

One-shot commands: `medicart-drive manual`, `medicart-drive auto`, `medicart-drive stop`, `medicart-drive status`.

## Settings (`/etc/medicart/control.env`, then `sudo systemctl restart medicart-control`)

| Key | Default | Meaning |
|---|---|---|
| `START_MODE` | `AUTO` | Mode after power-on when nothing is saved in Firebase |
| `MANUAL_MAX_PWM` | `160` | Fastest manual speed (firmware cap 200) |
| `MANUAL_SLOW_PWM` | `90` | Manual speed limit while `medicart.py` isn't running |
| `DRIVE_TIMEOUT_S` | `0.6` | Operator dead-man |
| `ARDUINO_PORT` | `auto` | Or a fixed path, e.g. `/dev/serial/by-id/usb-Arduino…` |

In the firmware (`medicart.ino`, top): `MANUAL_MAX_PWM 200`, `MANUAL_TIMEOUT_MS 500`, `MANUAL_ACCEL_PER_MS 1`, `MANUAL_DECEL_PER_MS 3`, `MANUAL_MOVE_BEEP false` (set `true` for a chirp every second while driving by hand).

## Running medicart.py by hand

The controller now holds the Arduino's port. To run Rifat's script in a terminal, either:
- **Share through the controller** (recommended):
  ```bash
  sudo systemctl stop medicart-vision
  MEDICART_SERIAL_PORT=/run/medicart/vision-tty MEDICART_VIDEO_SOURCE=10 ~/MideCart/venv/bin/python ~/MideCart/vision/medicart.py
  ```
- **Or use the port directly, as before:** `sudo systemctl stop medicart-control medicart-vision` first.

## Troubleshooting

| You see | Do this |
|---|---|
| Console Drive: **Controller offline** | `journalctl -u medicart-control -n 30`. *sign-in failed* → wrong cart password in `/etc/medicart/control.env`. No Wi-Fi → SSH driving still works. |
| **Manual** button greyed out, "needs firmware 2.1" | Upload `arduino/medicart/medicart.ino` (step 1). |
| Arduino **Not connected** | Replug USB; `ls /dev/ttyACM*`; check `sudo systemctl disable --now ModemManager` was done. |
| Vision **Not running** | `journalctl -u medicart-vision -n 30`. Port errors → check `MEDICART_SERIAL_PORT` is in `/etc/medicart/medicart.env` and `grep MEDICART_SERIAL_PORT ~/MideCart/vision/medicart.py` finds the 5 lines. |
| Manual: "Person ahead: forward is blocked" | That's medicart.py seeing someone within the stop distance. Turn or reverse, or wait. |
| Cart doesn't move in Auto | Is the mode **STOP**? Choose Auto. Is the line under the sensors? (Drive page → line sensors.) |
| Wheels turn the wrong way in Manual | Motor wires swapped: swap the motor leads, or the `MOTOR_*_PIN1/PIN2` numbers in the firmware. |

## Tests

```bash
bash tests/run_all.sh
```
- **Firmware (21 checks):** Rifat's Auto behaviour unchanged, manual ramp, cap, dead-man, STOP, Auto return, STAT, garbage input.
- **End to end (43 checks):** the real controller with the real firmware logic in real time, a fake `medicart.py` and a local Firebase stand-in. Covers Auto pass-through, Manual, person block, vision-offline limit, E-STOP latch and restart, SSH disconnect, controller crash, stale command after a restart, and Rifat's original firmware.
- **Browser (27 checks):** the built console and cart pages in Chromium, driving the same rig with mouse and keyboard.

All of this is simulated. Before patient use, repeat on the robot with the wheels lifted, then in a clear corridor:
- the dead-man time
- forward blocked for a person
- E-STOP
- the SSH disconnect stop
- the stopping distance at your `MANUAL_MAX_PWM`
