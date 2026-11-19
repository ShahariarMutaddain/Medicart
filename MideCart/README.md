# MideCart: the robot program (Raspberry Pi)

This folder is **Rifat Ahmed's original MideCart-of-IUB prototype**, the base MediCart 2.0 is built on. His files and behaviour are kept; additions are marked in the code (`[DASH-3]`, `[MC-3]`) and his originals are kept beside them. His own notes: [`README-rifat.md`](README-rifat.md).

| File | What it is |
|---|---|
| [`vision/medicart.py`](vision/medicart.py) | **The robot program.** YOLOv8n-pose person detection and distance (STOP ≤ 1 m, warning ≤ 2 m), ArUco/QR stations, the serial link to the Arduino, the dashboard. `medicart.py.original` = Rifat's version. |
| [`vision/web_dashboard.py`](vision/web_dashboard.py) | Rifat's Pi dashboard (Flask, port 5000), refined: password from other devices, firmware/sensor status, the same rules for every remote. `web_dashboard.rifat.py` = his original. |
| [`vision/drive_link.py`](vision/drive_link.py) | sends `STOP` / `MODE:AUTO` / `MODE:MANUAL` + `DRIVE` at the right rate, reads `FW` / `STAT` |
| [`vision/firebase_bridge.py`](vision/firebase_bridge.py) | connects the web console's Drive page through Firebase (sign-in with backoff) |
| [`vision/templates/`](vision/templates/), [`vision/static/`](vision/static/) | the dashboard page, works without internet |
| [`vision/opencv_human_distance_arucoqr.py.py`](vision/opencv_human_distance_arucoqr.py.py) | Rifat's original vision and calibration tool (press `C` to calibrate the distance) |
| `yolov8n-pose.pt`, `yolov8n.pt` | Ultralytics YOLOv8 weights (AGPL-3.0) |

## Run by hand

```bash
cd ~/MideCart && python3 -m venv venv && venv/bin/pip install ultralytics pyserial flask requests
cd vision && MEDICART_VIDEO_SOURCE=10 ../venv/bin/python medicart.py
# → [WEB] Dashboard available at http://127.0.0.1:5000   [ARDUINO] FW medicart 3.4
```

Normally it runs at boot as the `medicart-vision` service (see [`raspberry_pi/`](../raspberry_pi/)).

## Main settings (top of `medicart.py`)

| Constant | Value |
|---|---|
| `CONFIDENCE_THRESHOLD` | 0.35 |
| `STOP_DISTANCE` | 1.0 m |
| `WARNING_DISTANCE` | 2.0 m |
| `QR_CONFIRMATION_WINDOW` / `REQUIRED_SCANS` | 3 / 2 |
| camera | `MEDICART_VIDEO_SOURCE` (10 = the splitter's MediCart-Vision) |

Distances are only right after calibration (`camera_calibration.json`). See [`docs/HARDWARE.md`](../docs/HARDWARE.md#camera).
