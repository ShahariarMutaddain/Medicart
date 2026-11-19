# Arduino Mega firmware

The motion controller: line following, manual driving, smooth stop, the 750 ms watchdog, the buzzer and the station outputs. It takes orders from `medicart.py` on the Raspberry Pi over USB.

| File | What it is |
|---|---|
| [`medicart/medicart.ino`](medicart/medicart.ino) | **Firmware 3.4**, the one to upload. The team's LFR code unchanged (Nov 18: smooth stop) + reverse-turn assist (from the Nov 11 code) + the Pi protocol. |
| [`medicart_test/medicart_test.ino`](medicart_test/medicart_test.ino) | Bench tool without the Pi: live IR array viewer, calibration (`c`), invert (`i`), manual driving and line following. No camera brake. |
| [`medicart_lfr_original.ino.txt`](medicart_lfr_original.ino.txt) | The LFR code that ran on the robot (Nov 18), exactly as written. The tests compare firmware 3.4 against it. |
| [`medicart_lfr_nov11.ino.txt`](medicart_lfr_nov11.ino.txt) | The Nov 11 LFR code, source of the reverse turning. |
| [`history/`](history/) | Firmware 3.0–3.3, Rifat's final firmware and the original prototype code, kept unchanged. |

## Upload

1. Arduino IDE → **File → Open** → `medicart/medicart.ino`.
2. **Tools → Board → Arduino Mega or Mega 2560** → **Port** → **Upload**.
3. **Serial Monitor**, 115200: `MediCart ready`, `FW medicart 3.4`, then 5 lines a second:
   ```
   VIEW S1-S5 00100 | AUTO STOPPED | L 0 R 0 | pos 0.00 | hb 1200ms
   ```

Linux: `sudo usermod -aG dialout $USER` (log out and in). Arduino IDE as a snap: `sudo snap connect arduino:raw-usb`.

## Pins

| Signal | Pin |
|---|---|
| Left motor fwd / rev PWM | D2 / D3 |
| Right motor fwd / rev PWM | D4 / D5 |
| IR S1 … S5 | A8 … A12 (LOW = black line) |
| Buzzer | D25 |
| Stations 1–8 · 9–10 | D6–D13 · D22, D23 |

## Tuning (top of `medicart.ino`)

| Constant | Value | Meaning |
|---|---|---|
| `LINE_BASE_SPEED` | 50 | forward PWM while following |
| `LINE_STEERING_GAIN` / `LINE_MAX_STEERING` | 20 / 40 | proportional steering |
| `MOTOR_SLEW_RATE_PER_SECOND` | 300 | ramp for every speed change and every stop |
| `POSITION_FILTER_MS` | 25 | line-position smoothing |
| `LOST_LINE_SEARCH_MS` | 1500 | search towards the last side, then stop |
| `REVERSE_TURN_ASSIST` | `true` | inside wheel backwards on sharp turns; `false` = the Nov 18 code exactly |
| `COMMAND_TIMEOUT_MS` | 750 | no Pi command → stop |
| `MAX_PI_SPEED` | 180 | cap for `DRIVE:l,r` |

> [!IMPORTANT]
> After changing anything, run `bash tests/run_all.sh` and test on the robot with the wheels lifted first.

Protocol and keys: [`docs/PROTOCOLS.md`](../docs/PROTOCOLS.md#1-pi--arduino-serial).
