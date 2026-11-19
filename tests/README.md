# Tests (no hardware needed)

```bash
bash tests/run_all.sh
```

| # | Suite | How | Proves |
|:-:|---|---|---|
| 1 | **Firmware 3.4** | [`firmware/sim.cpp`](firmware/sim.cpp) compiles `medicart.ino` and the original LFR code on your PC with a fake `Arduino.h` | 240 000 ms of random line patterns give **the same PWM on pins 2–5, every millisecond**, as the team's LFR code (with and without reverse-turn assist), plus the Pi protocol, the 750 ms watchdog and the keys |
| 2 | **Bench sketch** | [`firmware/sim_test.cpp`](firmware/sim_test.cpp) | `medicart_test.ino`: viewer, calibration, keys, line following (30 checks) |
| 3 | **Robot end-to-end** | [`e2e/run_v3.py`](e2e/run_v3.py) + [`e2e/rig3.py`](e2e/rig3.py): the **real** `medicart.py` with simulated camera frames, a person switch instead of YOLO, firmware 3.4 running in real time on a virtual serial port ([`e2e/fwhost.cpp`](e2e/fwhost.cpp)), and a local stand-in for Firebase ([`e2e/fake_firebase.py`](e2e/fake_firebase.py)) | Auto, person stop, Manual, let-go stop, speed, the 180 cap, a person blocking Manual, emergency stop and its token, the Vercel console path, SSH, the password, a program crash while driving (42 checks) |
| 4 | **Browser UI** | [`browser/run_ui_v3.py`](browser/run_ui_v3.py), Playwright + Chromium, the built `index.html` / `cart.html` with stubbed Firebase SDK ([`browser/stub/`](browser/stub/)) | Drive page, cart banner, Pi dashboard, phone layout, every automatic voice trigger, no script errors (39 checks) |
| 5 | **Call + voice** | [`browser/run_call_voice.py`](browser/run_call_voice.py) | the cart rings, plays `0008`, auto-answers after it, and stays **silent during and after** the consultation (6 checks) |
| 6 | **ESP32 engine** | [`esp32/run.sh`](esp32/run.sh) extracts the oximeter engine from the sketch and runs it against a simulated module | heart-rate median, SpO₂ byte learning, motion hold, finger on/off |

## Requirements

```bash
sudo apt install g++ python3
pip install pyserial requests numpy opencv-python-headless flask
pip install playwright && playwright install chromium     # suites 4 and 5 (skipped without it)
```

Screenshots of every page: `python3 tests/browser/run_ui_v3.py --shots <folder>`.

> [!IMPORTANT]
> All of this is simulated. Before patient use, repeat on the robot: wheels lifted first, then a clear corridor. Check the let-go stop, the person stop in Auto and Manual, the emergency stop, unplugging USB while driving, and the stopping distance.
