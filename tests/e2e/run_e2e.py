"""End-to-end test of the MediCart control system, all real code except the hardware:

  fake medicart.py ──vision-tty──► medicart_control.py ──pty──► firmware 2.1 (real code, real time)
  fake console (HTTP) ──► fake Firebase ──SSE──►        ▲
  SSH client protocol ──unix socket────────────────────┘

Run:  python3 run_e2e.py
"""
import json
import os
import pty
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import tty

import requests
import serial

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DAEMON = os.path.join(ROOT, "raspberry_pi", "control", "medicart_control.py")
sys.path.insert(0, HERE)
from fake_firebase import make_server  # noqa: E402

RESULTS = []


def check(cond, what, detail=""):
    RESULTS.append((bool(cond), what))
    print(("PASS " if cond else "FAIL ") + what + (f"   [{detail}]" if detail else ""), flush=True)


def wait_until(pred, timeout=5.0, step=0.05):
    end = time.time() + timeout
    while time.time() < end:
        v = pred()
        if v:
            return v
        time.sleep(step)
    return pred()


class Rig:
    def __init__(self, fw="fwhost21"):
        self.tmp = tempfile.mkdtemp(prefix="mc-e2e-")
        self.motor_file = os.path.join(self.tmp, "motors")
        self.fw_bin = os.path.join(HERE, fw)
        self.srv, self.store = make_server()
        self.base = f"http://127.0.0.1:{self.srv.server_address[1]}"
        self.vision_cmd = None
        self.vision_stop = threading.Event()
        self.daemon = None
        self.fw = None
        self.start_fw()

    # firmware on a pseudo serial port
    def start_fw(self):
        self.m, self.s = pty.openpty()
        tty.setraw(self.s)
        self.port = os.ttyname(self.s)
        env = dict(os.environ, MOTOR_FILE=self.motor_file)
        self.fw = subprocess.Popen([self.fw_bin], stdin=self.m, stdout=self.m, env=env)

    def motors(self):
        try:
            l, r, _ = open(self.motor_file).read().split()
            return int(l), int(r)
        except (OSError, ValueError):
            return (0, 0)

    def start_daemon(self, extra=None):
        env = dict(os.environ,
                   MEDICART_CONTROL_ENV="/nonexistent",
                   CART_ID="medicart-01",
                   FIREBASE_API_KEY="test-key", FIREBASE_DB_URL=self.base,
                   FIREBASE_AUTH_BASE=self.base, FIREBASE_TOKEN_BASE=self.base,
                   CONTROL_EMAIL="cart01@medicart.iub", CONTROL_PASSWORD="secret-pass",
                   ARDUINO_PORT=self.port, ARDUINO_BOOT_WAIT_S="0.3",
                   VISION_TTY=os.path.join(self.tmp, "vision-tty"),
                   CONTROL_SOCKET=os.path.join(self.tmp, "control.sock"),
                   PUBLISH_INTERVAL_S="0.4", LOG_LEVEL="INFO")
        env.update(extra or {})
        self.log = open(os.path.join(self.tmp, "daemon.log"), "a")
        self.daemon = subprocess.Popen([sys.executable, DAEMON], env=env, stdout=self.log, stderr=subprocess.STDOUT)
        ok = wait_until(lambda: os.path.exists(os.path.join(self.tmp, "control.sock")), 5)
        return ok

    def stop_daemon(self, sig=signal.SIGTERM):
        if self.daemon:
            self.daemon.send_signal(sig)
            self.daemon.wait(5)
            self.daemon = None

    # fake medicart.py: opens the virtual port like Rifat's code (pyserial, exclusive)
    def start_vision(self):
        def run():
            port = None
            last = 0.0
            while not self.vision_stop.is_set():
                if port is None:
                    try:
                        port = serial.Serial(os.path.join(self.tmp, "vision-tty"), 115200, timeout=0.05, exclusive=True)
                    except (serial.SerialException, OSError):
                        time.sleep(0.2)
                        continue
                if self.vision_cmd and time.time() - last >= 0.35:
                    try:
                        port.write((self.vision_cmd + "\n").encode())
                    except (serial.SerialException, OSError):
                        port = None
                        continue
                    last = time.time()
                try:
                    port.read(256)            # drain "[ARDUINO]" lines like medicart.py does
                except (serial.SerialException, OSError):
                    port = None
                time.sleep(0.02)
        threading.Thread(target=run, daemon=True).start()

    # fake console writes
    def put(self, path, body):
        return requests.put(f"{self.base}/carts/medicart-01/control/{path}.json", params={"auth": "tok-console"},
                            data=json.dumps(body), timeout=5)

    def request(self, mode, rid):
        return self.put("request", {"mode": mode, "by": "uid-doc", "name": "Dr Test", "id": rid, "at": {".sv": "timestamp"}})

    def state(self):
        return self.store.get("/carts/medicart-01/control/state") or {}

    def sock(self):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(os.path.join(self.tmp, "control.sock"))
        f = s.makefile("rwb", buffering=0)

        def call(**m):
            f.write((json.dumps(m) + "\n").encode())
            return json.loads(f.readline())
        return s, call

    def close(self):
        self.vision_stop.set()
        self.stop_daemon()
        if self.fw:
            self.fw.kill()
        self.srv.shutdown()


def drive_for(rig, l, r, seconds, every=0.15, seq0=0):
    end = time.time() + seconds
    seq = seq0
    while time.time() < end:
        seq += 1
        rig.put("drive", {"l": l, "r": r, "seq": seq, "by": "uid-doc", "name": "Dr Test", "at": {".sv": "timestamp"}})
        time.sleep(every)
    return seq


def main():
    rig = Rig("fwhost21")
    try:
        check(rig.start_daemon(), "control service starts and opens its socket")
        check(wait_until(lambda: rig.state().get("arduino") == "online", 6), "Arduino link online (firmware on a serial port)")
        check(wait_until(lambda: rig.state().get("manualCapable"), 4), "firmware 2.1 detected → manual available",
              rig.state().get("firmware"))
        check(rig.state().get("mode") == "AUTO", "starts in AUTO (no saved request)")
        check(os.path.islink(os.path.join(rig.tmp, "vision-tty")), "virtual port for medicart.py exists")

        # ---------- AUTO: medicart.py drives, unchanged ----------
        rig.start_vision()
        rig.vision_cmd = "SAFE"
        check(wait_until(lambda: rig.motors() == (180, 180), 4), "AUTO: medicart.py SAFE passes through → line following", rig.motors())
        check(wait_until(lambda: rig.state().get("vision") == "clear", 3), "state shows vision clear")
        rig.vision_cmd = "STOP"
        t0 = time.time()
        check(wait_until(lambda: rig.motors() == (0, 0), 2), "AUTO: medicart.py STOP (person) stops the cart",
              f"{time.time() - t0:.2f}s")
        rig.vision_cmd = "SAFE"
        wait_until(lambda: rig.motors() == (180, 180), 3)

        # ---------- MANUAL from the web console ----------
        rig.request("MANUAL", "r1")
        check(wait_until(lambda: rig.state().get("mode") == "MANUAL", 3), "console request → MANUAL")
        check(wait_until(lambda: rig.motors() == (0, 0), 2), "entering MANUAL stops the line following")
        check(wait_until(lambda: rig.state().get("arduinoMode") == "MANUAL", 3), "Arduino reports mode=MANUAL (STAT)")
        seq = drive_for(rig, 150, 150, 1.2)
        check(rig.motors() == (150, 150), "MANUAL: console hold-to-drive 150/150 reaches the wheels", rig.motors())
        check(rig.state().get("driver") == "Dr Test", "state names the driver", rig.state().get("driver"))
        t0 = time.time()
        stopped = wait_until(lambda: rig.motors() == (0, 0), 2)
        check(stopped and time.time() - t0 < 1.0, "dead-man: console stops sending → wheels 0 in < 1 s", f"{time.time() - t0:.2f}s")
        seq = drive_for(rig, 120, -120, 0.8, seq0=seq)
        check(rig.motors() == (120, -120), "MANUAL: pivot on the spot", rig.motors())
        wait_until(lambda: rig.motors() == (0, 0), 2)

        # person ahead while driving manually
        rig.vision_cmd = "STOP"
        time.sleep(0.5)
        seq = drive_for(rig, 150, 150, 0.8, seq0=seq)
        check(rig.motors() == (0, 0), "MANUAL: person ahead → forward blocked", rig.motors())
        check(rig.state().get("blocked") is True, "state.blocked = true")
        seq = drive_for(rig, -100, -100, 0.8, seq0=seq)
        check(rig.motors() == (-100, -100), "MANUAL: reversing away still allowed", rig.motors())
        seq = drive_for(rig, 100, -100, 0.8, seq0=seq)
        check(rig.motors() == (100, -100), "MANUAL: turning on the spot still allowed", rig.motors())
        rig.vision_cmd = None              # medicart.py stops talking
        time.sleep(2.6)
        seq = drive_for(rig, 150, 150, 0.8, seq0=seq)
        check(rig.motors() == (90, 90), "MANUAL: vision offline → speed limited to MANUAL_SLOW_PWM", rig.motors())
        check(rig.state().get("vision") == "offline" and rig.state().get("limited"), "state shows vision offline + limited")
        rig.vision_cmd = "SAFE"
        wait_until(lambda: rig.motors() == (0, 0), 2)
        drive_for(rig, 250, 250, 1.0, seq0=seq)
        check(rig.motors() == (160, 160), "MANUAL: capped at MANUAL_MAX_PWM (160)", rig.motors())

        # ---------- E-STOP ----------
        rig.request("STOP", "r2")
        t0 = time.time()
        check(wait_until(lambda: rig.motors() == (0, 0), 1.5), "E-STOP from the console stops at once", f"{time.time() - t0:.2f}s")
        drive_for(rig, 150, 150, 0.6)
        check(rig.motors() == (0, 0) and rig.state().get("mode") == "STOP", "E-STOP latched: drive commands ignored")
        time.sleep(1.0)
        check(rig.motors() == (0, 0), "E-STOP: vision SAFE does not restart the cart")

        # ---------- back to AUTO ----------
        rig.request("AUTO", "r3")
        check(wait_until(lambda: rig.motors() == (180, 180), 3), "console AUTO → line following resumes on next SAFE", rig.motors())
        check(wait_until(lambda: rig.state().get("arduinoMode") == "AUTO", 3), "Arduino reports mode=AUTO")

        # ---------- SSH client protocol (no internet needed) ----------
        s, call = rig.sock()
        r = call(op="mode", mode="MANUAL", name="pi (terminal)")
        check(r.get("ok"), "SSH client: switch to MANUAL", r)
        check(wait_until(lambda: (rig.store.get("/carts/medicart-01/control/request") or {}).get("name") == "pi (terminal)", 3),
              "SSH mode change mirrored to Firebase (console sees it)")
        for _ in range(10):
            call(op="drive", l=110, r=110, name="pi (terminal)")
            time.sleep(0.1)
        check(rig.motors() == (110, 110), "SSH client: drive 110/110", rig.motors())
        for _ in range(3):
            call(op="drive", l=110, r=110, name="pi (terminal)")
            time.sleep(0.1)
        s.close()                          # SSH session dies while driving
        t0 = time.time()
        check(wait_until(lambda: rig.motors() == (0, 0), 1.5), "SSH client disconnects → wheels stop", f"{time.time() - t0:.2f}s")
        st = call.__closure__ and None
        s2, call2 = rig.sock()
        st = call2(op="status")["state"]
        check(st["mode"] == "MANUAL" and "events" in st, "status call works", st["mode"])
        s2.close()

        # ---------- crash safety ----------
        s3, call3 = rig.sock()
        for _ in range(6):
            call3(op="drive", l=140, r=140, name="pi (terminal)")
            time.sleep(0.1)
        rig.stop_daemon(signal.SIGKILL)    # control service crashes while driving
        t0 = time.time()
        check(wait_until(lambda: rig.motors() == (0, 0), 1.5), "service killed while driving → firmware stops wheels", f"{time.time() - t0:.2f}s")

        # ---------- restart: saved mode restored, stale drive ignored ----------
        rig.put("drive", {"l": 150, "r": 150, "seq": 999, "name": "stale"})
        rig.start_daemon()
        check(wait_until(lambda: rig.state().get("mode") == "MANUAL" and rig.state().get("arduino") == "online", 6),
              "restart: saved MANUAL request restored from Firebase")
        time.sleep(1.0)
        check(rig.motors() == (0, 0), "restart: stale stored drive command is ignored (no motion)", rig.motors())
        rig.request("STOP", "r4")
        wait_until(lambda: rig.state().get("mode") == "STOP", 3)
        rig.stop_daemon()
        rig.start_daemon()
        rig.vision_cmd = "SAFE"
        check(wait_until(lambda: rig.state().get("mode") == "STOP" and rig.state().get("arduino") == "online", 6),
              "restart: saved E-STOP stays latched")
        time.sleep(1.0)
        check(rig.motors() == (0, 0), "restart in E-STOP: cart stays still even with vision SAFE")
        rig.request("AUTO", "r5")
        check(wait_until(lambda: rig.motors() == (180, 180), 4), "release from E-STOP to AUTO works", rig.motors())
        events = [e["text"] for e in rig.state().get("events", [])]
        check(any("AUTO" in e for e in events), "events list published for the console", events[-2:] if events else "")
    finally:
        rig.close()

    # ---------- original firmware (no manual) ----------
    rig = Rig("fwhostorig")
    try:
        rig.start_daemon()
        wait_until(lambda: rig.state().get("arduino") == "online", 6)
        time.sleep(1.2)
        check(not rig.state().get("manualCapable"), "Rifat's original firmware: manual not offered", rig.state().get("firmware"))
        rig.start_vision()
        rig.vision_cmd = "SAFE"
        check(wait_until(lambda: rig.motors() == (180, 180), 4), "original firmware: AUTO still works through the service")
        rig.request("MANUAL", "o1")
        time.sleep(1.0)
        ev = [e["text"] for e in rig.state().get("events", [])]
        check(rig.state().get("mode") == "AUTO" and any("firmware 2.1" in e for e in ev),
              "original firmware: MANUAL request refused with a clear reason", ev[-1:] if ev else "")
        rig.request("STOP", "o2")
        check(wait_until(lambda: rig.motors() == (0, 0), 2), "original firmware: E-STOP works")
    finally:
        rig.close()

    bad = [w for ok, w in RESULTS if not ok]
    print(f"\n{len(RESULTS) - len(bad)}/{len(RESULTS)} passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
