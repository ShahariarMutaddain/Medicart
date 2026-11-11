"""MediCart web dashboard (Rifat's design) — local control page served by medicart.py.

Modes: "stopped" (start-up and after an emergency stop), "auto" (line following, the
camera only brakes) and "manual" (an operator drives). medicart.py asks
requested_drive(safety_command) every loop and sends the answer to the Arduino:
    STOP | MODE:AUTO | MODE:MANUAL + DRIVE:<left>,<right>

Changes for MediCart 3.0 are marked  [MC-3]:
  * the same rules serve three front-ends: this page, the Vercel console (Firebase bridge)
    and SSH (medicart-drive, through this HTTP API on 127.0.0.1)
  * a password protects the page when it is opened from another device
  * status also reports the Arduino firmware, line sensors, the driver and recent events
"""
from __future__ import annotations

import base64
import hmac
import logging
import os
import threading
import time
from math import isfinite
from typing import Callable, Optional

import cv2
import numpy as np
from flask import Flask, Response, jsonify, render_template, request
from werkzeug.serving import BaseWSGIServer, make_server


MAX_MANUAL_SPEED = 180
# [MC-3.1] manual speed at start-up; the line follower runs at PWM 38, so manual starts low too
DEFAULT_MANUAL_SPEED = max(0, min(MAX_MANUAL_SPEED, int(os.environ.get("MANUAL_SPEED", "40") or 40)))
MANUAL_CONTROL_LEASE_SECONDS = 0.5
REMOTE_CONTROL_LEASE_SECONDS = 0.7   # [MC-3] internet commands arrive with more jitter
_VALID_MODES = {"auto", "manual"}
STREAM_MAX_WIDTH = 640               # [MC-3] smaller video for the page: less CPU on the Pi
STREAM_FPS = 12


class WebDashboard:
    def __init__(self, start_mode: str = "stopped") -> None:
        self._lock = threading.Lock()
        self._frame: Optional[np.ndarray] = None
        self._frame_time = 0.0
        self._mode = start_mode if start_mode in ("stopped", "auto", "manual") else "stopped"
        self._mode_by = "start-up"
        self._stop_token = 0
        self._speed = DEFAULT_MANUAL_SPEED   # [MC-3.1] was 110
        self._forward = 0.0
        self._turn = 0.0
        self._keys: tuple[str, ...] = ()
        self._last_control_time = 0.0
        # [MC-3] direct wheel targets (Vercel console / SSH) and who is driving
        self._direct: Optional[tuple[int, int]] = None
        self._direct_time = 0.0
        self._driver = ""
        self._blocked = False
        self._vision_status = "STOP"
        self._people_count = 0
        self._nearest_distance: Optional[float] = None
        self._fps = 0.0
        self._camera_fps = 0.0
        self._frame_latency_ms: Optional[float] = None
        self._inference_ms: Optional[float] = None
        self._motor_left = 0
        self._motor_right = 0
        self._serial_state = "starting"
        self._firmware = ""
        self._line = ""
        self._line_lost = False
        self._events: list[dict] = []
        self.on_change: list[Callable[[str, str], None]] = []   # (what, detail)
        self._password = os.environ.get("DASHBOARD_PASSWORD", "")
        self._server: Optional[BaseWSGIServer] = None
        self._thread: Optional[threading.Thread] = None

        here = os.path.dirname(os.path.abspath(__file__))
        self.app = Flask(__name__, template_folder=os.path.join(here, "templates"),
                         static_folder=os.path.join(here, "static"))
        self._register_routes()

    # ── [MC-3] events + auth ─────────────────────────────────────────
    def _event(self, text: str) -> None:
        self._events.append({"t": int(time.time() * 1000), "text": text})
        self._events = self._events[-10:]
        print(f"[DASHBOARD] {text}", flush=True)

    def _notify(self, what: str, detail: str = "") -> None:
        for cb in list(self.on_change):
            try:
                cb(what, detail)
            except Exception:  # noqa: BLE001
                pass

    def _authorised(self) -> bool:
        if request.remote_addr in ("127.0.0.1", "::1"):
            return True                       # on the Pi itself (SSH tool, kiosk)
        if not self._password:
            return False
        header = request.headers.get("Authorization", "")
        if header.startswith("Basic "):
            try:
                _user, _, pw = base64.b64decode(header[6:]).decode("utf-8").partition(":")
            except (ValueError, UnicodeDecodeError):
                return False
            return hmac.compare_digest(pw, self._password)
        return False

    def _register_routes(self) -> None:
        @self.app.before_request
        def guard():
            if not self._authorised():
                return Response("MediCart dashboard: password required", 401,
                                {"WWW-Authenticate": 'Basic realm="MediCart"'})
            return None

        @self.app.get("/")
        def index():
            return render_template("dashboard.html")

        @self.app.get("/video_feed")
        def video_feed():
            return Response(
                self._video_frames(),
                mimetype="multipart/x-mixed-replace; boundary=frame",
                headers={"Cache-Control": "no-store"},
            )

        @self.app.get("/api/status")
        def status():
            with self._lock:
                return jsonify({
                    "mode": self._mode,
                    "mode_by": self._mode_by,
                    "stop_token": self._stop_token,
                    "speed": self._speed,
                    "vision": self._vision_status,
                    "people": self._people_count,
                    "nearest_distance": self._nearest_distance,
                    "fps": round(self._fps, 1),
                    "camera_fps": round(self._camera_fps, 1),
                    "frame_latency_ms": self._frame_latency_ms,
                    "inference_ms": self._inference_ms,
                    "motor_left": self._motor_left,
                    "motor_right": self._motor_right,
                    "keys": "+".join(key.upper() for key in self._keys) or "---",
                    "serial": self._serial_state,
                    "max_speed": MAX_MANUAL_SPEED,
                    "firmware": self._firmware,
                    "line": self._line,
                    "line_lost": self._line_lost,
                    "driver": self._driver,
                    "blocked": self._blocked,
                    "video_age": round(time.monotonic() - self._frame_time, 1) if self._frame_time else None,
                    "events": list(self._events),
                })

        @self.app.post("/api/mode")
        def set_mode():
            payload = request.get_json(silent=True) or {}
            if not isinstance(payload, dict):
                return jsonify({"error": "request body must be a JSON object"}), 400
            mode = payload.get("mode")
            if not isinstance(mode, str) or mode not in _VALID_MODES:
                return jsonify({"error": "mode must be auto or manual"}), 400
            try:
                stop_token = int(payload.get("stop_token"))
            except (TypeError, ValueError):
                return jsonify({"error": "stop_token is required"}), 400
            who = str(payload.get("name") or "Pi dashboard")[:40]
            with self._lock:
                if stop_token != self._stop_token:
                    return jsonify({"error": "mode request predates emergency stop"}), 409
                self._apply_mode(mode, who)
            self._notify("mode", who)
            return jsonify({"mode": mode})

        @self.app.post("/api/stop")
        def stop():
            payload = request.get_json(silent=True) or {}
            who = str((payload or {}).get("name") or "Pi dashboard")[:40] if isinstance(payload, dict) else "Pi dashboard"
            stop_token = self.emergency_stop(who)
            return jsonify({"mode": "stopped", "stop_token": stop_token})

        @self.app.post("/api/speed")
        def set_speed():
            payload = request.get_json(silent=True) or {}
            if not isinstance(payload, dict):
                return jsonify({"error": "request body must be a JSON object"}), 400
            speed_value = payload.get("speed")
            if isinstance(speed_value, bool) or (
                isinstance(speed_value, float) and not speed_value.is_integer()
            ):
                return jsonify({"error": "speed must be an integer"}), 400
            try:
                speed = int(speed_value)
            except (TypeError, ValueError):
                return jsonify({"error": "speed must be an integer"}), 400
            if not 0 <= speed <= MAX_MANUAL_SPEED:
                return jsonify({"error": f"speed must be between 0 and {MAX_MANUAL_SPEED}"}), 400
            with self._lock:
                self._speed = speed
            return jsonify({"speed": speed})

        @self.app.post("/api/control")
        def control():
            payload = request.get_json(silent=True) or {}
            if not isinstance(payload, dict):
                return jsonify({"error": "request body must be a JSON object"}), 400
            try:
                forward = float(payload.get("forward", 0))
                turn = float(payload.get("turn", 0))
            except (TypeError, ValueError):
                return jsonify({"error": "forward and turn must be numeric"}), 400
            if not (isfinite(forward) and isfinite(turn)
                    and -1.0 <= forward <= 1.0 and -1.0 <= turn <= 1.0):
                return jsonify({"error": "forward and turn must be between -1 and 1"}), 400
            keys = payload.get("keys", [])
            valid_keys = {"up", "down", "left", "right"}
            if not isinstance(keys, list) or any(
                not isinstance(key, str) or key not in valid_keys for key in keys
            ):
                return jsonify({"error": "keys must contain only up, down, left, or right"}), 400
            who = str(payload.get("name") or "Pi dashboard")[:40]
            with self._lock:
                if self._mode != "manual":
                    return jsonify({"error": "select manual mode before driving"}), 409
                self._forward = forward
                self._turn = turn
                self._keys = tuple(sorted(set(keys)))
                self._last_control_time = time.monotonic()
                self._direct = None                      # [MC-3] the newest source wins
                if forward or turn:
                    self._driver = who
            return jsonify({"forward": forward, "turn": turn})

    # ── [MC-3] internal mode change (lock held) ─────────────────────
    def _apply_mode(self, mode: str, who: str) -> None:
        previous = self._mode
        self._mode = mode
        self._mode_by = who
        self._forward = 0.0
        self._turn = 0.0
        self._keys = ()
        self._last_control_time = 0.0
        self._direct = None
        self._direct_time = 0.0
        if previous != mode:
            self._event(f"Mode {previous} -> {mode} ({who})")

    def _video_frames(self):
        last = None
        while True:
            with self._lock:
                frame = None if self._frame is None else self._frame
                stamp = self._frame_time
            if frame is None or stamp == last:
                time.sleep(0.05)
                continue
            last = stamp
            encoded, buffer = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 75])
            if encoded:
                yield b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" + buffer.tobytes() + b"\r\n"
            time.sleep(1.0 / STREAM_FPS)

    def publish_frame(
        self,
        frame: np.ndarray,
        vision_status: str,
        people_count: int,
        nearest_distance: Optional[float],
        fps: float,
        serial_state: str,
        camera_fps: float = 0.0,
        frame_latency_ms: Optional[float] = None,
        inference_ms: Optional[float] = None,
        motor_left: int = 0,
        motor_right: int = 0,
    ) -> None:
        h, w = frame.shape[:2]
        if w > STREAM_MAX_WIDTH:                         # [MC-3] shrink once here, not per viewer
            frame = cv2.resize(frame, (STREAM_MAX_WIDTH, int(h * STREAM_MAX_WIDTH / w)), interpolation=cv2.INTER_AREA)
        else:
            frame = frame.copy()
        with self._lock:
            self._frame = frame
            self._frame_time = time.monotonic()
            self._vision_status = vision_status
            self._people_count = people_count
            self._nearest_distance = nearest_distance
            self._fps = fps
            self._camera_fps = camera_fps
            self._frame_latency_ms = frame_latency_ms
            self._inference_ms = inference_ms
            self._motor_left = motor_left
            self._motor_right = motor_right
            self._serial_state = serial_state

    # [MC-3] what the Arduino reports (firmware line, STAT / DBG motor lines)
    def set_arduino_info(self, firmware: Optional[str] = None, line: Optional[str] = None,
                         line_lost: Optional[bool] = None, motor: Optional[tuple[int, int]] = None,
                         serial_state: Optional[str] = None) -> None:
        with self._lock:
            if firmware is not None and firmware != self._firmware:
                self._firmware = firmware
                self._event(f"Arduino firmware: {firmware}")
            if line is not None:
                self._line = line
            if line_lost is not None:
                self._line_lost = line_lost
            if motor is not None:
                self._motor_left, self._motor_right = motor
            if serial_state is not None:
                self._serial_state = serial_state

    def requested_drive(self, safety_command: str) -> tuple[str, int, int]:
        with self._lock:
            mode = self._mode
            speed = self._speed
            forward = self._forward
            turn = self._turn
            last_control_time = self._last_control_time
            direct = self._direct
            direct_time = self._direct_time

        if mode == "stopped" or safety_command == "STOP":
            with self._lock:
                # [MC-3] tell the operators why the cart is not moving
                self._blocked = mode == "manual" and safety_command == "STOP"
            return "STOP", 0, 0
        with self._lock:
            self._blocked = False
        if mode == "auto":
            return "MODE:AUTO", 0, 0

        now = time.monotonic()
        # [MC-3] wheel speeds from the Vercel console / SSH, with their own lease
        if direct is not None and now - direct_time <= REMOTE_CONTROL_LEASE_SECONDS:
            left, right = direct
        else:
            if now - last_control_time > MANUAL_CONTROL_LEASE_SECONDS:
                return "MODE:MANUAL", 0, 0
            left = round((forward + turn) * speed)
            right = round((forward - turn) * speed)
        peak = max(abs(left), abs(right), MAX_MANUAL_SPEED)
        if peak > MAX_MANUAL_SPEED:
            scale = MAX_MANUAL_SPEED / peak
            left = round(left * scale)
            right = round(right * scale)
        return "MODE:MANUAL", left, right

    def control_snapshot(self) -> dict[str, object]:
        with self._lock:
            return {
                "mode": self._mode,
                "keys": "+".join(key.upper() for key in self._keys) or "---",
            }

    def emergency_stop(self, who: str = "Pi dashboard") -> int:
        with self._lock:
            self._stop_token += 1
            previous = self._mode
            self._mode = "stopped"
            self._mode_by = who
            self._forward = 0.0
            self._turn = 0.0
            self._keys = ()
            self._last_control_time = 0.0
            self._direct = None
            self._event(f"EMERGENCY STOP ({who})" if previous != "stopped" else f"Stop confirmed ({who})")
            token = self._stop_token
        self._notify("stop", who)
        return token

    # ── [MC-3] remote front-ends (Firebase bridge, SSH tool) ─────────
    def remote_mode(self, mode: str, who: str) -> tuple[bool, str]:
        """An explicit request from the Vercel console: 'auto', 'manual' or 'stopped'."""
        mode = (mode or "").lower()
        mode = {"stop": "stopped"}.get(mode, mode)
        if mode == "stopped":
            self.emergency_stop(who)
            return True, "stopped"
        if mode not in _VALID_MODES:
            return False, f"unknown mode {mode!r}"
        with self._lock:
            self._apply_mode(mode, who)
        return True, mode

    def remote_drive(self, left: float, right: float, who: str) -> tuple[bool, str]:
        with self._lock:
            if self._mode != "manual":
                return False, "select manual mode before driving"
            l = int(max(-MAX_MANUAL_SPEED, min(MAX_MANUAL_SPEED, float(left))))
            r = int(max(-MAX_MANUAL_SPEED, min(MAX_MANUAL_SPEED, float(right))))
            self._direct = (l, r)
            self._direct_time = time.monotonic()
            if l or r:
                self._driver = who
        return True, "ok"

    def snapshot(self) -> dict:
        """Everything the Firebase bridge publishes (no video)."""
        with self._lock:
            return {
                "mode": self._mode, "mode_by": self._mode_by, "speed": self._speed,
                "vision": self._vision_status, "people": self._people_count,
                "nearest_distance": self._nearest_distance, "fps": self._fps,
                "motor_left": self._motor_left, "motor_right": self._motor_right,
                "serial": self._serial_state, "firmware": self._firmware,
                "line": self._line, "line_lost": self._line_lost, "driver": self._driver,
                "blocked": self._blocked, "events": list(self._events),
                "video_age": (time.monotonic() - self._frame_time) if self._frame_time else None,
            }

    def start(self, host: str = "127.0.0.1", port: int = 5000) -> bool:
        if host not in ("127.0.0.1", "localhost", "::1") and not self._password:
            # [MC-3] never offer the controls to the whole network without a password
            print("[WEB] DASHBOARD_PASSWORD is not set: the dashboard stays on 127.0.0.1 only", flush=True)
            host = "127.0.0.1"
        logging.getLogger("werkzeug").setLevel(logging.WARNING)   # [MC-3] no log line per status poll
        try:
            self._server = make_server(host, port, self.app, threaded=True)
        except OSError as exc:
            print(f"[WEB] Could not start dashboard at http://{host}:{port}: {exc}", flush=True)
            return False
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        self._thread.start()
        print(f"[WEB] Dashboard available at http://{host}:{port}", flush=True)
        return True

    def stop(self) -> None:
        if self._server is not None:
            self._server.shutdown()
            self._server.server_close()
            self._server = None
        if self._thread is not None:
            self._thread.join(timeout=2)
            self._thread = None
