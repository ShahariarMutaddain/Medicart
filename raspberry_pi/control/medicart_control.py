#!/usr/bin/env python3
# ════════════════════════════════════════════════════════════════════════════
#  medicart-control — MediCart drive controller (Raspberry Pi)
#  IUB Robotics Team
#
#  One process owns the Arduino Mega's USB serial port and decides who may move
#  the wheels:
#
#    AUTO    Rifat's medicart.py drives. It talks to a virtual serial port
#            (/run/medicart/vision-tty) and every line it sends (SAFE / STOP /
#            QR:n / RESET_QR) is passed to the Arduino unchanged. The Arduino
#            follows the line exactly as before, including its 2 s watchdog.
#    MANUAL  An operator drives: the web console (Firebase) or the SSH keyboard
#            client (medicart-drive). Wheel speeds go to the Arduino as
#            MAN:<left>,<right> every 100 ms. No fresh command for 0.6 s -> 0,0;
#            the Arduino stops by itself 0.5 s after the last MAN: as a backstop.
#            If medicart.py reports a person in the stop zone (STOP), forward
#            motion is blocked (turning on the spot and reversing still work).
#    STOP    Emergency stop, latched. STOP is sent every 250 ms. Only an
#            explicit AUTO or MANUAL request releases it.
#
#  Needs firmware 2.1 on the Mega for MANUAL (prints "FW medicart-ctrl 2.1").
#  With Rifat's original firmware, AUTO and STOP still work; MANUAL is refused.
#
#  Python 3.9+, pyserial, requests  (sudo apt install python3-serial python3-requests)
#  Settings: /etc/medicart/control.env  (see control.env.example)
# ════════════════════════════════════════════════════════════════════════════
import errno
import fcntl
import glob
import json
import logging
import os
import pty
import select
import signal
import socket
import sys
import termios
import threading
import time
import tty
from typing import Optional

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    print("pyserial missing: sudo apt install python3-serial", file=sys.stderr)
    raise
try:
    import requests
except ImportError:  # pragma: no cover
    print("requests missing: sudo apt install python3-requests", file=sys.stderr)
    raise

VERSION = "2.1.0"
MODES = ("AUTO", "MANUAL", "STOP")
log = logging.getLogger("medicart-control")


# ─── Settings ───────────────────────────────────────────────────────────────
def load_env_file(path: str) -> None:
    """KEY=VALUE lines -> os.environ (values already in the environment win)."""
    if not path or not os.path.isfile(path):
        return
    with open(path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, val = line.split("=", 1)
            val = val.strip().strip('"').strip("'")
            os.environ.setdefault(key.strip(), val)


def env(key: str, default: str = "") -> str:
    return os.environ.get(key, default).strip()


def env_f(key: str, default: float) -> float:
    try:
        return float(env(key, str(default)))
    except ValueError:
        return default


class Settings:
    def __init__(self) -> None:
        self.cart_id = env("CART_ID", "medicart-01")
        self.api_key = env("FIREBASE_API_KEY")
        self.db_url = env("FIREBASE_DB_URL").rstrip("/")
        self.email = env("CONTROL_EMAIL")
        self.password = env("CONTROL_PASSWORD")
        self.auth_base = env("FIREBASE_AUTH_BASE", "https://identitytoolkit.googleapis.com").rstrip("/")
        self.token_base = env("FIREBASE_TOKEN_BASE", "https://securetoken.googleapis.com").rstrip("/")
        self.arduino_port = env("ARDUINO_PORT", "auto")
        self.vision_tty = env("VISION_TTY", "/run/medicart/vision-tty")
        self.socket_path = env("CONTROL_SOCKET", "/run/medicart/control.sock")
        self.start_mode = env("START_MODE", "AUTO").upper()
        self.max_pwm = int(env_f("MANUAL_MAX_PWM", 160))
        self.slow_pwm = int(env_f("MANUAL_SLOW_PWM", 90))
        self.drive_timeout = env_f("DRIVE_TIMEOUT_S", 0.6)
        self.vision_offline_s = env_f("VISION_OFFLINE_S", 2.0)
        self.block_hold_s = env_f("BLOCK_HOLD_S", 1.0)
        self.publish_s = env_f("PUBLISH_INTERVAL_S", 1.0)
        if self.start_mode not in MODES:
            self.start_mode = "AUTO"
        self.max_pwm = max(0, min(self.max_pwm, 200))
        self.slow_pwm = max(0, min(self.slow_pwm, self.max_pwm))

    @property
    def firebase_enabled(self) -> bool:
        return bool(self.api_key and self.db_url and self.email and self.password)


def clamp(v: float, lo: float, hi: float) -> float:
    return lo if v < lo else hi if v > hi else v


# ─── Controller: the single source of truth ─────────────────────────────────
class Controller:
    def __init__(self, cfg: Settings) -> None:
        self.cfg = cfg
        self.lock = threading.RLock()
        self.mode = cfg.start_mode
        self.mode_by = "start-up"
        self.mode_at = time.time()
        self.drive_l = 0
        self.drive_r = 0
        self.drive_t = 0.0
        self.drive_by = ""
        self.vision_cmd: Optional[str] = None
        self.vision_t = 0.0
        self.vision_stop_t = 0.0
        self.applied = (0, 0)
        self.blocked = False
        self.limited = False
        self.events: list = []          # recent events for the console (last 8)
        self.arduino = None             # ArduinoLink
        self.vision = None              # VisionBridge
        self.on_change = []             # callbacks(reason)

    # ---- helpers --------------------------------------------------------
    def event(self, text: str) -> None:
        log.info(text)
        with self.lock:
            self.events.append({"t": int(time.time() * 1000), "text": text})
            self.events = self.events[-8:]
        for cb in self.on_change:
            try:
                cb("event")
            except Exception:  # noqa: BLE001
                pass

    def vision_state(self, now: Optional[float] = None) -> str:
        now = now or time.monotonic()
        if self.vision_t == 0 or now - self.vision_t > self.cfg.vision_offline_s:
            return "offline"
        if now - self.vision_stop_t < self.cfg.block_hold_s:
            return "blocked"
        return "clear"

    # ---- inputs ---------------------------------------------------------
    def set_mode(self, mode: str, by: str = "?") -> tuple:
        mode = (mode or "").upper()
        if mode not in MODES:
            return False, f"unknown mode {mode!r}"
        ard = self.arduino
        with self.lock:
            if mode == "MANUAL" and ard is not None and ard.connected and not ard.manual_capable:
                return False, "Arduino firmware 2.1 is needed for manual driving"
            prev = self.mode
            self.mode, self.mode_by, self.mode_at = mode, by, time.time()
            self.drive_l = self.drive_r = 0
            self.drive_t = 0.0
            self.applied = (0, 0)
        if ard is not None:
            if mode == "STOP":
                ard.send("STOP")
            elif mode == "MANUAL":
                ard.send("STOP")            # halt whatever was happening first
                if ard.manual_capable:
                    ard.send("MAN:0,0")     # enter manual on the Arduino, wheels still
            elif mode == "AUTO":
                if ard.manual_capable:
                    ard.send("AUTO")        # stops, waits for medicart.py's next SAFE
        if prev != mode:
            self.event(f"Mode {prev} -> {mode} ({by})")
        else:
            self.event(f"Mode {mode} confirmed ({by})")
        return True, mode

    def set_drive(self, l: float, r: float, by: str = "?") -> tuple:
        with self.lock:
            if self.mode != "MANUAL":
                return False, f"not in MANUAL (mode is {self.mode})"
            self.drive_l = int(clamp(float(l), -255, 255))
            self.drive_r = int(clamp(float(r), -255, 255))
            self.drive_t = time.monotonic()
            self.drive_by = by
        return True, "ok"

    def release_drive(self, by: str) -> None:
        with self.lock:
            if self.drive_by == by:
                self.drive_l = self.drive_r = 0
                self.drive_t = 0.0

    def on_vision_line(self, line: str) -> None:
        """A command from medicart.py arrived on the virtual port."""
        cmd = line.strip()
        if not cmd:
            return
        now = time.monotonic()
        ard = self.arduino
        if cmd in ("SAFE", "STOP"):
            with self.lock:
                was_offline = self.vision_state(now) == "offline"
                self.vision_cmd, self.vision_t = cmd, now
                if cmd == "STOP":
                    self.vision_stop_t = now
                mode = self.mode
            if was_offline:
                self.event("Vision (medicart.py) connected")
            if mode == "AUTO" and ard is not None:
                ard.send(cmd)               # AUTO: pass through untouched
        elif cmd.startswith("QR:") or cmd == "RESET_QR":
            if ard is not None:
                ard.send(cmd)               # bed outputs work in every mode
        else:
            log.debug("vision sent unknown line %r (ignored)", cmd)

    # ---- the 20 Hz loop -------------------------------------------------
    def tick(self, now: float) -> None:
        ard = self.arduino
        with self.lock:
            mode = self.mode
            fresh = self.drive_t and (now - self.drive_t) <= self.cfg.drive_timeout
            l, r = (self.drive_l, self.drive_r) if fresh else (0, 0)
            vstate = self.vision_state(now)
        if ard is None or not ard.connected:
            with self.lock:
                self.applied, self.blocked, self.limited = (0, 0), False, False
            return
        if ard.due("stat", 0.5, now):
            ard.send("STAT")
        if mode == "STOP":
            if ard.due("stop", 0.25, now):
                ard.send("STOP")
            with self.lock:
                self.applied, self.blocked, self.limited = (0, 0), False, False
        elif mode == "MANUAL":
            if not ard.manual_capable:
                if ard.due("stop", 0.25, now):
                    ard.send("STOP")
                with self.lock:
                    self.applied = (0, 0)
                return
            limit = self.cfg.slow_pwm if vstate == "offline" else self.cfg.max_pwm
            l, r = int(clamp(l, -limit, limit)), int(clamp(r, -limit, limit))
            blocked = False
            v, w = (l + r) / 2.0, (l - r) / 2.0
            if vstate == "blocked" and v > 0:
                blocked = True              # person ahead: drop the forward part, keep turning
                l, r = int(round(w)), int(round(-w))
            if ard.due("man", 0.1, now) or (l, r) != self.applied:
                ard.send(f"MAN:{l},{r}")
                ard.mark("man", now)
            with self.lock:
                self.applied = (l, r)
                self.blocked = blocked
                self.limited = vstate == "offline" and (l or r) != 0
        else:  # AUTO
            with self.lock:
                self.applied, self.blocked, self.limited = (0, 0), False, False

    # ---- snapshot for Firebase / socket ----------------------------------
    def snapshot(self) -> dict:
        ard = self.arduino
        now = time.monotonic()
        with self.lock:
            stat = dict(ard.stat) if ard else {}
            return {
                "mode": self.mode,
                "modeBy": self.mode_by,
                "modeAt": int(self.mode_at * 1000),
                "driver": self.drive_by.split("#")[0] if self.mode == "MANUAL" else "",
                "applied": {"l": self.applied[0], "r": self.applied[1]},
                "blocked": self.blocked,
                "limited": self.limited,
                "vision": self.vision_state(now),
                "arduino": "online" if (ard and ard.connected) else "offline",
                "arduinoPort": (ard.port_name if ard else "") or "",
                "firmware": (ard.firmware if ard else "") or "",
                "manualCapable": bool(ard and ard.manual_capable),
                "line": stat.get("line", ""),
                "lineLost": stat.get("lost") == "1",
                "obstacle": stat.get("obstacle") == "1",
                "arduinoMode": stat.get("mode", ""),
                "maxPwm": self.cfg.max_pwm,
                "slowPwm": self.cfg.slow_pwm,
                "events": list(self.events),
                "version": VERSION,
            }


# ─── Arduino link: owns the real USB serial port ────────────────────────────
class ArduinoLink(threading.Thread):
    def __init__(self, ctl: Controller, cfg: Settings) -> None:
        super().__init__(name="arduino", daemon=True)
        self.ctl, self.cfg = ctl, cfg
        self.ser: Optional[serial.Serial] = None
        self.port_name = ""
        self.connected = False
        self.firmware = ""
        self.manual_capable = False
        self.stat: dict = {}
        self.wlock = threading.Lock()
        self._due: dict = {}
        self.stop_flag = threading.Event()

    def find_port(self) -> Optional[str]:
        if self.cfg.arduino_port and self.cfg.arduino_port != "auto":
            return self.cfg.arduino_port if os.path.exists(self.cfg.arduino_port) else None
        pats = ["/dev/serial/by-id/*Arduino*", "/dev/serial/by-id/*2341*", "/dev/serial/by-id/*1a86*",
                "/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyUSB0", "/dev/ttyUSB1"]
        for pat in pats:
            for p in sorted(glob.glob(pat)):
                return p
        return None

    def due(self, key: str, every: float, now: float) -> bool:
        last = self._due.get(key, 0.0)
        if now - last >= every:
            self._due[key] = now
            return True
        return False

    def mark(self, key: str, now: float) -> None:
        self._due[key] = now

    def send(self, line: str) -> bool:
        with self.wlock:
            ser = self.ser
            if ser is None or not self.connected:
                return False
            try:
                ser.write((line + "\n").encode("ascii"))
                return True
            except (serial.SerialException, OSError) as exc:
                log.warning("serial write failed: %s", exc)
                self._drop()
                return False

    def _drop(self) -> None:
        was = self.connected
        self.connected = False
        try:
            if self.ser:
                self.ser.close()
        except Exception:  # noqa: BLE001
            pass
        self.ser = None
        if was:
            self.ctl.event("Arduino disconnected")

    def run(self) -> None:
        buf = bytearray()
        while not self.stop_flag.is_set():
            if not self.connected:
                port = self.find_port()
                if not port:
                    time.sleep(2)
                    continue
                try:
                    try:
                        ser = serial.Serial(port, 115200, timeout=0.1, write_timeout=0.5, exclusive=True)
                    except TypeError:
                        ser = serial.Serial(port, 115200, timeout=0.1, write_timeout=0.5)
                except (serial.SerialException, OSError) as exc:
                    log.warning("cannot open %s: %s", port, exc)
                    time.sleep(2)
                    continue
                with self.wlock:
                    self.ser, self.port_name = ser, port
                    self.firmware, self.manual_capable, self.stat = "", False, {}
                    buf.clear()
                time.sleep(float(env("ARDUINO_BOOT_WAIT_S", "2.5")))   # opening the port resets the Mega
                self.connected = True
                self.ctl.event(f"Arduino connected on {port}")
                self.send("STAT")
                mode = self.ctl.mode
                if mode == "STOP":
                    self.send("STOP")
            try:
                chunk = self.ser.read(256) if self.ser else b""
            except (serial.SerialException, OSError) as exc:
                log.warning("serial read failed: %s", exc)
                with self.wlock:
                    self._drop()
                time.sleep(1)
                continue
            if self.port_name and not os.path.exists(self.port_name):
                with self.wlock:
                    self._drop()
                continue
            if not chunk:
                continue
            buf.extend(chunk)
            while b"\n" in buf:
                i = buf.index(b"\n")
                raw = bytes(buf[:i])
                del buf[:i + 1]
                self.on_line(raw.decode("utf-8", "ignore").strip())
            if len(buf) > 1024:
                buf.clear()

    def on_line(self, line: str) -> None:
        if not line:
            return
        if line.startswith("STAT "):
            kv = dict(p.split("=", 1) for p in line[5:].split() if "=" in p)
            first = not self.manual_capable
            self.stat = kv
            self.manual_capable = True      # only firmware 2.1 answers STAT
            if first:
                self.firmware = self.firmware or "medicart-ctrl 2.1"
                self.ctl.event("Firmware 2.1 detected: manual drive available")
                # put the Arduino in the mode we are in (it may still be in MANUAL from before a restart)
                mode = self.ctl.mode
                self.send({"AUTO": "AUTO", "MANUAL": "MAN:0,0", "STOP": "STOP"}[mode])
            return                          # status replies are not for medicart.py
        if line.startswith("FW "):
            self.firmware = line[3:].strip()
            if "ctrl" in self.firmware:
                self.manual_capable = True
        if line == "MediCart ready" and not self.firmware:
            self.firmware = "original (no manual drive)"
        if self.ctl.vision is not None:
            self.ctl.vision.forward(line)  # medicart.py still sees "[ARDUINO] ..." lines


# ─── Vision bridge: virtual serial port for medicart.py ────────────────────
class VisionBridge(threading.Thread):
    def __init__(self, ctl: Controller, cfg: Settings) -> None:
        super().__init__(name="vision", daemon=True)
        self.ctl, self.cfg = ctl, cfg
        self.master, self.slave = pty.openpty()
        tty.setraw(self.slave)                       # no echo, no line editing
        attrs = termios.tcgetattr(self.slave)
        attrs[3] &= ~termios.ECHO
        termios.tcsetattr(self.slave, termios.TCSANOW, attrs)
        fl = fcntl.fcntl(self.master, fcntl.F_GETFL)
        fcntl.fcntl(self.master, fcntl.F_SETFL, fl | os.O_NONBLOCK)
        self.slave_name = os.ttyname(self.slave)
        os.chmod(self.slave_name, 0o660)
        link = cfg.vision_tty
        os.makedirs(os.path.dirname(link), exist_ok=True)
        tmp = link + ".new"
        try:
            os.unlink(tmp)
        except FileNotFoundError:
            pass
        os.symlink(self.slave_name, tmp)
        os.replace(tmp, link)
        log.info("medicart.py port: %s -> %s", link, self.slave_name)

    def forward(self, line: str) -> None:
        # only while medicart.py is actually talking, so stale lines don't pile up
        if self.ctl.vision_state() == "offline":
            return
        try:
            os.write(self.master, (line + "\n").encode("utf-8"))
        except OSError as exc:
            if exc.errno not in (errno.EAGAIN, errno.EIO):
                log.debug("vision write: %s", exc)

    def run(self) -> None:
        buf = bytearray()
        while True:
            r, _, _ = select.select([self.master], [], [], 1.0)
            if not r:
                continue
            try:
                data = os.read(self.master, 1024)
            except OSError as exc:
                if exc.errno in (errno.EAGAIN, errno.EIO):
                    time.sleep(0.2)
                    continue
                raise
            if not data:
                time.sleep(0.1)
                continue
            buf.extend(data)
            while True:
                idx = -1
                for sep in (b"\n", b"\r"):
                    j = buf.find(sep)
                    if j != -1 and (idx == -1 or j < idx):
                        idx = j
                if idx == -1:
                    break
                raw = bytes(buf[:idx])
                del buf[:idx + 1]
                self.ctl.on_vision_line(raw.decode("utf-8", "ignore"))
            if len(buf) > 512:
                buf.clear()


# ─── Firebase (REST + streaming) ────────────────────────────────────────────
class Firebase:
    """Signs in as the cart account, listens to carts/<id>/control and publishes state."""

    def __init__(self, ctl: Controller, cfg: Settings) -> None:
        self.ctl, self.cfg = ctl, cfg
        self.http = requests.Session()
        self.id_token = ""
        self.refresh_token = ""
        self.uid = ""
        self.expires = 0.0
        self.tlock = threading.Lock()
        self.online = False
        self.last_request_id = None
        self.base = f"{cfg.db_url}/carts/{cfg.cart_id}/control"
        self._kick = threading.Event()
        self.reconnect = threading.Event()

    # ---- auth ----------------------------------------------------------
    def token(self) -> str:
        with self.tlock:
            if self.id_token and time.time() < self.expires - 300:
                return self.id_token
            try:
                if self.refresh_token:
                    r = self.http.post(f"{self.cfg.token_base}/v1/token?key={self.cfg.api_key}",
                                       data={"grant_type": "refresh_token", "refresh_token": self.refresh_token},
                                       timeout=15)
                    if r.ok:
                        j = r.json()
                        self.id_token, self.refresh_token = j["id_token"], j["refresh_token"]
                        self.expires = time.time() + int(j.get("expires_in", 3600))
                        return self.id_token
                r = self.http.post(f"{self.cfg.auth_base}/v1/accounts:signInWithPassword?key={self.cfg.api_key}",
                                   json={"email": self.cfg.email, "password": self.cfg.password,
                                         "returnSecureToken": True}, timeout=15)
                if not r.ok:
                    msg = r.json().get("error", {}).get("message", r.text[:120]) if r.content else r.status_code
                    raise RuntimeError(f"sign-in failed: {msg}")
                j = r.json()
                self.id_token, self.refresh_token, self.uid = j["idToken"], j["refreshToken"], j.get("localId", "")
                self.expires = time.time() + int(j.get("expiresIn", 3600))
                log.info("signed in to Firebase as %s", self.cfg.email)
                return self.id_token
            except requests.RequestException as exc:
                raise RuntimeError(f"network: {exc}") from exc

    # ---- writes --------------------------------------------------------
    def patch(self, path: str, data: dict) -> bool:
        try:
            r = self.http.patch(f"{self.base}{path}.json", params={"auth": self.token()},
                                data=json.dumps(data), timeout=10)
            if not r.ok:
                log.warning("Firebase write %s -> %s %s", path, r.status_code, r.text[:120])
            return r.ok
        except (RuntimeError, requests.RequestException) as exc:
            log.warning("Firebase write failed: %s", exc)
            return False

    def write_request(self, mode: str, by: str) -> None:
        """Local (SSH) mode changes are mirrored so the console and restarts agree."""
        rid = f"pi-{int(time.time() * 1000)}"
        self.last_request_id = rid
        threading.Thread(target=self.patch, args=("", {"request": {
            "mode": mode, "by": "pi", "name": by, "id": rid, "at": {".sv": "timestamp"}}}),
            daemon=True).start()

    def publisher(self) -> None:
        while True:
            self._kick.wait(self.cfg.publish_s)
            self._kick.clear()
            state = self.ctl.snapshot()
            state["updatedAt"] = {".sv": "timestamp"}
            state["online"] = True
            self.patch("", {"state": state})

    def kick(self, _reason: str = "") -> None:
        self._kick.set()

    # ---- stream --------------------------------------------------------
    def handle(self, path: str, data, snapshot: bool) -> None:
        if path == "/":
            data = data or {}
            if "request" in data:
                self.handle("/request", data.get("request"), snapshot)
            if "drive" in data and not snapshot:
                self.handle("/drive", data.get("drive"), snapshot)
            return
        if path.startswith("/state"):
            return                                   # our own writes coming back
        if path == "/request" or path.startswith("/request/"):
            req = data if path == "/request" else None
            if req is None:                          # a sub-field changed: re-read the whole request
                req = self.get("/request")
            if not isinstance(req, dict):
                return
            rid = req.get("id")
            if rid and rid == self.last_request_id:
                return
            self.last_request_id = rid
            who = req.get("name") or req.get("by") or "console"
            ok, msg = self.ctl.set_mode(str(req.get("mode", "")), f"{who}{' (saved)' if snapshot else ''}")
            if not ok:
                self.ctl.event(f"Request refused: {msg}")
            self.kick()
            return
        if path == "/drive" or path.startswith("/drive/"):
            if snapshot:
                return                               # never act on a stale stored drive command
            d = data if path == "/drive" else self.get("/drive")
            if isinstance(d, dict):
                self.ctl.set_drive(d.get("l", 0) or 0, d.get("r", 0) or 0, str(d.get("name") or "console"))

    def get(self, path: str):
        try:
            r = self.http.get(f"{self.base}{path}.json", params={"auth": self.token()}, timeout=10)
            return r.json() if r.ok else None
        except (RuntimeError, requests.RequestException, ValueError):
            return None

    def stream(self) -> None:
        backoff = 2
        while True:
            try:
                tok = self.token()
                with self.http.get(f"{self.base}.json", params={"auth": tok},
                                   headers={"Accept": "text/event-stream"},
                                   stream=True, timeout=(10, 75)) as r:
                    if r.status_code != 200:
                        raise RuntimeError(f"stream HTTP {r.status_code}: {r.text[:120]}")
                    if not self.online:
                        self.ctl.event("Firebase connected")
                    self.online, backoff = True, 2
                    first = True
                    event = None
                    for raw in r.iter_lines(decode_unicode=True):
                        if raw is None:
                            continue
                        if raw.startswith("event:"):
                            event = raw[6:].strip()
                        elif raw.startswith("data:"):
                            payload = raw[5:].strip()
                            if event in ("put", "patch"):
                                msg = json.loads(payload)
                                path, data = msg.get("path", "/"), msg.get("data")
                                if event == "patch" and isinstance(data, dict):
                                    for k, v in data.items():
                                        self.handle(("" if path == "/" else path) + "/" + k, v, False)
                                else:
                                    self.handle(path, data, first and path == "/")
                                first = False
                            elif event == "auth_revoked":
                                with self.tlock:
                                    self.expires = 0
                                raise RuntimeError("token expired, reconnecting")
                            elif event == "cancel":
                                raise RuntimeError(f"stream cancelled (rules?): {payload}")
                        if self.reconnect.is_set():
                            self.reconnect.clear()
                            break
                        if time.time() > self.expires - 240:
                            break                    # refresh the token before it expires
            except Exception as exc:  # noqa: BLE001
                if self.online:
                    self.ctl.event(f"Firebase disconnected ({exc})")
                else:
                    log.warning("Firebase: %s", exc)
                self.online = False
                time.sleep(backoff)
                backoff = min(backoff * 2, 60)

    def start(self) -> None:
        threading.Thread(target=self.stream, name="fb-stream", daemon=True).start()
        threading.Thread(target=self.publisher, name="fb-publish", daemon=True).start()
        self.ctl.on_change.append(self.kick)


# ─── Local control socket (medicart-drive, scripts) ─────────────────────────
class LocalSocket(threading.Thread):
    """JSON lines on a Unix socket. Only the medicart user (and root) can connect."""

    def __init__(self, ctl: Controller, cfg: Settings, fb: Optional[Firebase]) -> None:
        super().__init__(name="socket", daemon=True)
        self.ctl, self.cfg, self.fb = ctl, cfg, fb
        path = cfg.socket_path
        os.makedirs(os.path.dirname(path), exist_ok=True)
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(path)
        os.chmod(path, 0o660)
        self.srv.listen(4)

    def run(self) -> None:
        n = 0
        while True:
            conn, _ = self.srv.accept()
            n += 1
            threading.Thread(target=self.client, args=(conn, f"terminal-{n}"), daemon=True).start()

    def client(self, conn: socket.socket, cid: str) -> None:
        f = conn.makefile("rwb", buffering=0)
        name = "Pi terminal"
        try:
            for raw in f:
                try:
                    msg = json.loads(raw.decode("utf-8"))
                except ValueError:
                    f.write(b'{"ok":false,"error":"bad json"}\n')
                    continue
                op = msg.get("op")
                if msg.get("name"):
                    name = str(msg["name"])[:40]
                if op == "mode":
                    ok, info = self.ctl.set_mode(str(msg.get("mode", "")), name)
                    if ok and self.fb is not None:
                        self.fb.write_request(info, name)
                    reply = {"ok": ok, "info": info}
                elif op == "drive":
                    ok, info = self.ctl.set_drive(msg.get("l", 0), msg.get("r", 0), f"{name}#{cid}")
                    reply = {"ok": ok, "info": info}
                elif op == "status":
                    reply = {"ok": True, "state": self.ctl.snapshot()}
                else:
                    reply = {"ok": False, "error": f"unknown op {op!r}"}
                f.write((json.dumps(reply) + "\n").encode("utf-8"))
        except (OSError, ValueError):
            pass
        finally:
            self.ctl.release_drive(f"{name}#{cid}")    # client gone -> its drive command is void
            try:
                conn.close()
            except OSError:
                pass


# ─── main ──────────────────────────────────────────────────────────────────
def main() -> int:
    load_env_file(os.environ.get("MEDICART_CONTROL_ENV", "/etc/medicart/control.env"))
    logging.basicConfig(level=os.environ.get("LOG_LEVEL", "INFO"),
                        format="%(asctime)s %(levelname)s %(message)s", datefmt="%H:%M:%S")
    cfg = Settings()
    log.info("medicart-control %s starting (cart %s, start mode %s)", VERSION, cfg.cart_id, cfg.start_mode)

    ctl = Controller(cfg)
    ctl.vision = VisionBridge(ctl, cfg)
    ctl.arduino = ArduinoLink(ctl, cfg)
    fb = None
    if cfg.firebase_enabled:
        fb = Firebase(ctl, cfg)
    else:
        log.warning("Firebase settings missing in control.env: web control disabled (SSH control still works)")
    sock = LocalSocket(ctl, cfg, fb)

    ctl.vision.start()
    ctl.arduino.start()
    sock.start()
    if fb:
        fb.start()

    stop = threading.Event()

    def bye(*_a):
        stop.set()

    signal.signal(signal.SIGTERM, bye)
    signal.signal(signal.SIGINT, bye)

    period = 0.05
    while not stop.is_set():
        ctl.tick(time.monotonic())
        stop.wait(period)

    # leaving: make sure the wheels are still
    log.info("stopping: sending STOP")
    if ctl.arduino and ctl.arduino.connected:
        ctl.arduino.send("STOP")
    return 0


if __name__ == "__main__":
    sys.exit(main())
