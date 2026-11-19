"""End-to-end test of MediCart 3.0: the real medicart.py + Rifat's dashboard + Firebase bridge
+ firmware 3.0 (real code, real time). Only camera, YOLO, Arduino hardware and Firebase are faked.

Run:  python3 run_v3.py   (needs fwhost3 built: see tests/run_all.sh)
"""
import os
import subprocess
import sys
import time

import requests

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from rig3 import Rig3, wait_until, ROOT  # noqa: E402

RESULTS = []


def check(cond, what, detail=""):
    RESULTS.append((bool(cond), what))
    print(("PASS " if cond else "FAIL ") + what + (f"   [{detail}]" if detail != "" else ""), flush=True)


def hold(rig, seconds, every=0.1, **ctl):
    end = time.time() + seconds
    while time.time() < end:
        rig.api("/api/control", dict({"forward": 0, "turn": 0, "keys": []}, **ctl))
        time.sleep(every)


def console_drive(rig, l, r, seconds, every=0.15):
    end, seq = time.time() + seconds, 0
    while time.time() < end:
        seq += 1
        rig.put("drive", {"l": l, "r": r, "seq": seq, "name": "Dr Test", "at": {".sv": "timestamp"}})
        time.sleep(every)


def main():
    rig = Rig3(os.path.join(HERE, "fwhost3"))
    try:
        rig.start_robot()
        def up():
            try:
                return requests.get(f"{rig.dash}/api/status", timeout=1).ok
            except requests.RequestException:
                return False
        check(wait_until(up, 15), "medicart.py starts Rifat's dashboard")
        st = rig.status()
        check(st["mode"] == "stopped", "starts STOPPED (Rifat's default)", st["mode"])
        check(wait_until(lambda: rig.status()["serial"] == "connected", 6), "Arduino connected over serial")
        check(wait_until(lambda: "3.4" in rig.status()["firmware"], 6), "firmware 3.4 detected", rig.status()["firmware"])
        check(wait_until(lambda: rig.status()["line"] == "00100", 4), "line sensors reported (STAT)", rig.status()["line"])
        time.sleep(1.0)
        check(rig.motors() == (0, 0), "stopped at start: wheels still")
        check(wait_until(lambda: rig.state().get("mode") == "STOP" and rig.state().get("manualCapable"), 6),
              "Vercel console sees STOP + manual available (Firebase state)")
        check((rig.store.get("/carts/medicart-01/control/request") or {}).get("name") == "start-up",
              "start-up mode written for the console")

        # ---- AUTO through Rifat's API ----
        code, _ = rig.mode("auto")
        check(code == 200, "local dashboard: Auto accepted")
        check(wait_until(lambda: rig.motors() == (50, 50), 3), "AUTO: line following (centre = 50, the LFR speed)", rig.motors())
        rig.person = "close"
        t0 = time.time()
        check(wait_until(lambda: rig.motors() == (0, 0), 3), "AUTO: person close -> stop", f"{time.time() - t0:.2f}s")
        check(wait_until(lambda: rig.state().get("vision") == "blocked", 3), "console sees vision blocked")
        rig.person = "far"
        check(wait_until(lambda: rig.motors() == (50, 50), 4), "AUTO: person far away -> continues", rig.motors())
        rig.person = None

        # ---- MANUAL through Rifat's API ----
        rig.mode("manual")
        check(wait_until(lambda: rig.motors() == (0, 0), 2), "MANUAL: line following stops")
        hold(rig, 1.0, forward=1, keys=["up"])
        check(rig.motors() == (40, 40), "MANUAL: hold forward -> 40 (default speed)", rig.motors())
        t0 = time.time()
        check(wait_until(lambda: rig.motors() == (0, 0), 2) and time.time() - t0 < 1.4,
              "MANUAL: let go -> stops (0.5 s lease + 0.75 s firmware)", f"{time.time() - t0:.2f}s")
        rig.api("/api/speed", {"speed": 150})
        hold(rig, 0.8, forward=0, turn=1, keys=["right"])
        check(rig.motors() == (150, -150), "MANUAL: speed 150, turn right on the spot", rig.motors())
        wait_until(lambda: rig.motors() == (0, 0), 2)
        code, body = rig.api("/api/speed", {"speed": 999})
        check(code == 400, "speed above 180 refused")
        rig.person = "close"
        time.sleep(0.6)
        hold(rig, 0.8, forward=1, keys=["up"])
        check(rig.motors() == (0, 0) and rig.status()["blocked"], "MANUAL: person in the stop zone -> no movement, blocked shown")
        rig.person = None
        time.sleep(1.2)

        # ---- EMERGENCY STOP + stop token ----
        token = rig.status()["stop_token"]
        hold(rig, 0.5, forward=1)
        rig.api("/api/stop", {"name": "tester"})
        check(wait_until(lambda: rig.motors() == (0, 0), 1.5), "EMERGENCY STOP from the dashboard")
        code, body = rig.api("/api/mode", {"mode": "auto", "stop_token": token})
        check(code == 409, "an old page cannot undo an emergency stop (stop token)", body.get("error"))
        check(wait_until(lambda: rig.state().get("mode") == "STOP", 3), "console sees EMERGENCY STOP")
        check(wait_until(lambda: (rig.store.get("/carts/medicart-01/control/request") or {}).get("mode") == "STOP", 3),
              "local stop mirrored to the console request")

        # ---- Vercel console (Firebase) ----
        rig.request("MANUAL", "c1")
        check(wait_until(lambda: rig.status()["mode"] == "manual", 4), "console: Manual request applied")
        console_drive(rig, 120, 120, 1.2)
        check(rig.motors() == (120, 120), "console: hold-to-drive 120/120", rig.motors())
        check(rig.status()["driver"] == "Dr Test", "driver name shown", rig.status()["driver"])
        t0 = time.time()
        check(wait_until(lambda: rig.motors() == (0, 0), 2.5), "console: stops after the doctor lets go",
              f"{time.time() - t0:.2f}s")
        console_drive(rig, 250, -250, 0.8)
        check(rig.motors() == (180, -180), "console: capped at 180", rig.motors())
        rig.request("AUTO", "c2")
        check(wait_until(lambda: rig.motors() == (50, 50), 4), "console: Auto -> line following")
        rig.request("STOP", "c3")
        check(wait_until(lambda: rig.motors() == (0, 0), 2) and rig.status()["mode"] == "stopped", "console: EMERGENCY STOP")
        console_drive(rig, 120, 120, 0.6)
        check(rig.motors() == (0, 0), "console: drive ignored while stopped")
        events = [e["text"] for e in rig.state().get("events", [])]
        check(any("Dr Test" in e for e in events), "events published", events[-1] if events else "")

        # ---- SSH tool ----
        env = dict(os.environ, DASHBOARD_PORT=str(rig.port))
        tool = os.path.join(ROOT, "raspberry_pi", "control", "medicart-drive")
        out = subprocess.run([sys.executable, tool, "auto"], env=env, capture_output=True, text=True, timeout=10)
        check(wait_until(lambda: rig.status()["mode"] == "auto", 3), "SSH: medicart-drive auto", out.stdout.strip()[:60])
        out = subprocess.run([sys.executable, tool, "status"], env=env, capture_output=True, text=True, timeout=10)
        check('"mode": "auto"' in out.stdout, "SSH: medicart-drive status")
        subprocess.run([sys.executable, tool, "stop"], env=env, capture_output=True, text=True, timeout=10)
        check(wait_until(lambda: rig.status()["mode"] == "stopped", 3), "SSH: medicart-drive stop")

        # ---- page + password ----
        r = requests.get(f"{rig.dash}/", timeout=3)
        check(r.ok and "MediCart Pi Dashboard" in r.text, "dashboard page served")
        check(requests.get(f"{rig.dash}/static/dashboard.js", timeout=3).ok, "dashboard script served")
        app = rig.mc.WebDashboard().app if hasattr(rig.mc, "WebDashboard") else None
        c = app.test_client()
        check(c.get("/api/status", environ_base={"REMOTE_ADDR": "192.168.1.50"}).status_code == 401, "other devices need the password")
        import base64
        auth = {"Authorization": "Basic " + base64.b64encode(b"x:dash-pass").decode()}
        check(c.get("/api/status", headers=auth, environ_base={"REMOTE_ADDR": "192.168.1.50"}).status_code == 200,
              "correct password works")
        check(c.get("/api/status", headers={"Authorization": "Basic " + base64.b64encode(b"x:nope").decode()},
                    environ_base={"REMOTE_ADDR": "192.168.1.50"}).status_code == 401, "wrong password refused")

        # ---- program stops while driving ----
        rig.mode("manual")
        hold(rig, 1.0, forward=1, keys=["up"])   # firmware ramps at 300 PWM/s: 0 -> 150 takes 0.5 s
        check(rig.motors() == (150, 150), "driving before the crash (speed 150 set earlier)", rig.motors())
        # keep the lease alive so only the program exit can stop it
        import threading
        stop_hold = threading.Event()
        threading.Thread(target=lambda: [rig.api("/api/control", {"forward": 1, "turn": 0, "keys": ["up"]}) or time.sleep(0.1)
                                         for _ in iter(lambda: stop_hold.is_set(), True)], daemon=True).start()
        time.sleep(0.4)
        rig.stop_robot()
        t0 = time.time()
        check(wait_until(lambda: rig.motors() == (0, 0), 2), "medicart.py exits while driving -> Arduino stops by itself",
              f"{time.time() - t0:.2f}s")
        stop_hold.set()
    finally:
        rig.close()
    bad = [w for ok, w in RESULTS if not ok]
    print(f"\n{len(RESULTS) - len(bad)}/{len(RESULTS)} passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
