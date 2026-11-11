"""Test rig for MediCart 3.0: runs the REAL medicart.py main loop with only the hardware faked.

  fake camera frames ─► medicart.main() (real: safety logic, Rifat's WebDashboard, DriveLink,
                                         FirebaseBridge, serial reconnect, drawing)
                         │ serial (pty)
                         ▼
               firmware 3.0 (the real medicart.ino compiled for the PC, running in real time)

  YOLO is replaced by a switch: rig.person = None | "close" | "far".
  Firebase is the local stand-in from fake_firebase.py.
"""
import json
import os
import pty
import socket
import subprocess
import sys
import tempfile
import threading
import time
import tty

import numpy as np
import requests

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
VISION = os.path.join(ROOT, "MideCart", "vision")
sys.path.insert(0, HERE)
from fake_firebase import make_server  # noqa: E402


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class FakeCamera:
    def __init__(self, rig):
        self.rig = rig

    def read(self):
        if self.rig.camera_stop.is_set():
            return False, None
        time.sleep(1 / 30)
        frame = np.full((720, 1280, 3), 200, np.uint8)
        frame[300:420, 600:680] = (40, 40, 40)
        return True, frame

    def release(self):
        pass

    def isOpened(self):
        return True


class Rig3:
    def __init__(self, fw_bin, extra_env=None):
        self.tmp = tempfile.mkdtemp(prefix="mc3-")
        self.motor_file = os.path.join(self.tmp, "motors")
        self.srv, self.store = make_server()
        self.base = f"http://127.0.0.1:{self.srv.server_address[1]}"
        self.port = free_port()
        self.dash = f"http://127.0.0.1:{self.port}"
        self.person = None
        self.camera_stop = threading.Event()
        self.thread = None
        self.log_lines = []
        # firmware on a pseudo serial port
        self.m, self.s = pty.openpty()
        tty.setraw(self.s)
        self.serial_port = os.ttyname(self.s)
        self.fw = subprocess.Popen([fw_bin], stdin=self.m, stdout=self.m,
                                   env=dict(os.environ, MOTOR_FILE=self.motor_file))
        env = {
            "CART_ID": "medicart-01", "FIREBASE_API_KEY": "test-key", "FIREBASE_DB_URL": self.base,
            "FIREBASE_AUTH_BASE": self.base, "FIREBASE_TOKEN_BASE": self.base,
            "CONTROL_EMAIL": "cart01@medicart.iub", "CONTROL_PASSWORD": "secret-pass",
            "DASHBOARD_HOST": "127.0.0.1", "DASHBOARD_PORT": str(self.port),
            "DASHBOARD_PASSWORD": "dash-pass", "START_MODE": "stopped", "PUBLISH_INTERVAL_S": "0.4",
        }
        env.update(extra_env or {})
        os.environ.update(env)

    # ---- the real medicart.py with fake hardware -------------------------
    def start_robot(self):
        if VISION not in sys.path:
            sys.path.insert(0, VISION)
        for mod in ("medicart", "web_dashboard", "drive_link", "firebase_bridge"):
            sys.modules.pop(mod, None)
        import medicart  # noqa: E402
        self.mc = medicart
        rig = self

        def fake_detect(model, frame, focal_length, device, track_labels, next_label):
            time.sleep(0.03)
            if rig.person is None:
                return [], next_label
            close = rig.person == "close"
            d = 0.6 if close else 3.5
            h = 700 if close else 200
            return [medicart.PersonDetection("P1", (500, 0, 800, h), (650, h // 2), 300, h, 0.9, d,
                                             "center", "middle", close=close)], next_label

        medicart.initialize_camera = lambda: FakeCamera(rig)
        medicart.load_model = lambda: (None, "cpu")
        medicart.load_calibration = lambda: 1000.0
        medicart.detect_people = fake_detect
        medicart.find_serial_port = lambda: rig.serial_port
        medicart.is_display_available = lambda: False
        medicart.SERIAL_BOOT_WAIT_SECONDS = 0.3
        medicart.SERIAL_RETRY_INTERVAL_SECONDS = 0.5
        self.thread = threading.Thread(target=medicart.main, name="medicart-main", daemon=True)
        self.thread.start()

    def stop_robot(self):
        self.camera_stop.set()
        if self.thread:
            self.thread.join(10)

    # ---- observations ------------------------------------------------------
    def motors(self):
        try:
            l, r, _ = open(self.motor_file).read().split()
            return int(l), int(r)
        except (OSError, ValueError):
            return (0, 0)

    def state(self):
        return self.store.get("/carts/medicart-01/control/state") or {}

    def status(self):
        return requests.get(f"{self.dash}/api/status", timeout=3).json()

    def api(self, path, body):
        r = requests.post(f"{self.dash}{path}", json=body, timeout=3)
        return r.status_code, (r.json() if r.content else {})

    def mode(self, mode):
        return self.api("/api/mode", {"mode": mode, "stop_token": self.status()["stop_token"], "name": "tester"})

    # ---- fake Vercel console ---------------------------------------------
    def put(self, path, body):
        return requests.put(f"{self.base}/carts/medicart-01/control/{path}.json", params={"auth": "tok-console"},
                            data=json.dumps(body), timeout=5)

    def request(self, mode, rid):
        return self.put("request", {"mode": mode, "by": "uid-doc", "name": "Dr Test", "id": rid, "at": {".sv": "timestamp"}})

    def close(self):
        self.stop_robot()
        try:
            self.fw.kill()
        except OSError:
            pass
        self.srv.shutdown()


def wait_until(pred, timeout=5.0, step=0.05):
    end = time.time() + timeout
    while time.time() < end:
        v = pred()
        if v:
            return v
        time.sleep(step)
    return pred()
