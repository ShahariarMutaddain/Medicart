"""MediCart 3.0 — Firebase bridge: the Vercel console's Drive page <-> WebDashboard.

Reads   carts/<id>/control/request  {mode AUTO|MANUAL|STOP, name, id}
        carts/<id>/control/drive    {l, r, name}  (every 150 ms while a doctor holds a key)
Writes  carts/<id>/control/state    once a second (mode, vision, Arduino, line sensors, events)

Signs in with the cart account. Settings come from the environment (/etc/medicart/control.env):
    CART_ID, FIREBASE_API_KEY, FIREBASE_DB_URL, CONTROL_EMAIL, CONTROL_PASSWORD
Without them the bridge stays off and the local dashboard + SSH still work.
Needs: requests (pip install requests).
"""
from __future__ import annotations

import json
import os
import threading
import time
from typing import Optional

try:
    import requests
except ImportError:  # pragma: no cover
    requests = None

VERSION = "3.1.0"
MODE_TO_FB = {"auto": "AUTO", "manual": "MANUAL", "stopped": "STOP"}


def _env(key: str, default: str = "") -> str:
    return os.environ.get(key, default).strip()


class FirebaseBridge:
    def __init__(self, dashboard) -> None:
        self.dash = dashboard
        self.cart = _env("CART_ID", "medicart-01")
        self.api_key = _env("FIREBASE_API_KEY")
        self.db = _env("FIREBASE_DB_URL").rstrip("/")
        self.email = _env("CONTROL_EMAIL")
        self.password = _env("CONTROL_PASSWORD")
        self.auth_base = _env("FIREBASE_AUTH_BASE", "https://identitytoolkit.googleapis.com").rstrip("/")
        self.token_base = _env("FIREBASE_TOKEN_BASE", "https://securetoken.googleapis.com").rstrip("/")
        self.publish_s = float(_env("PUBLISH_INTERVAL_S", "1.0") or 1.0)
        self.base = f"{self.db}/carts/{self.cart}/control"
        self.http = requests.Session() if requests else None
        self.id_token = ""
        self.refresh_token = ""
        self.expires = 0.0
        self.tlock = threading.Lock()
        self.online = False
        self.last_request_id: Optional[str] = None
        self.applying = False
        self._kick = threading.Event()
        self._stop = threading.Event()

    @property
    def enabled(self) -> bool:
        return bool(self.http and self.api_key and self.db and self.email and self.password)

    # ── auth ───────────────────────────────────────────────────────────
    def token(self) -> str:
        with self.tlock:
            if self.id_token and time.time() < self.expires - 300:
                return self.id_token
            try:
                if self.refresh_token:
                    r = self.http.post(f"{self.token_base}/v1/token?key={self.api_key}",
                                       data={"grant_type": "refresh_token", "refresh_token": self.refresh_token},
                                       timeout=15)
                    if r.ok:
                        j = r.json()
                        self.id_token, self.refresh_token = j["id_token"], j["refresh_token"]
                        self.expires = time.time() + int(j.get("expires_in", 3600))
                        return self.id_token
                r = self.http.post(f"{self.auth_base}/v1/accounts:signInWithPassword?key={self.api_key}",
                                   json={"email": self.email, "password": self.password, "returnSecureToken": True},
                                   timeout=15)
                if not r.ok:
                    msg = r.json().get("error", {}).get("message", "") if r.content else r.status_code
                    raise RuntimeError(f"sign-in failed: {msg}")
                j = r.json()
                self.id_token, self.refresh_token = j["idToken"], j["refreshToken"]
                self.expires = time.time() + int(j.get("expiresIn", 3600))
                print(f"[FIREBASE] signed in as {self.email}", flush=True)
                return self.id_token
            except requests.RequestException as exc:
                raise RuntimeError(f"network: {exc}") from exc

    def patch(self, path: str, data: dict) -> bool:
        try:
            r = self.http.patch(f"{self.base}{path}.json", params={"auth": self.token()},
                                data=json.dumps(data), timeout=10)
            return r.ok
        except (RuntimeError, requests.RequestException) as exc:
            print(f"[FIREBASE] write failed: {exc}", flush=True)
            return False

    def get(self, path: str):
        try:
            r = self.http.get(f"{self.base}{path}.json", params={"auth": self.token()}, timeout=10)
            return r.json() if r.ok else None
        except (RuntimeError, requests.RequestException, ValueError):
            return None

    # ── state out ──────────────────────────────────────────────────────
    def state(self) -> dict:
        s = self.dash.snapshot()
        video_age = s.get("video_age")
        if video_age is None or video_age > 3.0:
            vision = "offline"
        elif s["vision"] in ("STOP", "PERSON_DETECTED"):
            vision = "blocked"
        else:
            vision = "clear"
        fw = s.get("firmware") or ""
        return {
            "mode": MODE_TO_FB.get(s["mode"], "STOP"),
            "modeBy": s.get("mode_by", ""),
            "driver": s.get("driver", "") if s["mode"] == "manual" else "",
            "applied": {"l": s["motor_left"], "r": s["motor_right"]},
            "blocked": bool(s.get("blocked")),
            "limited": False,
            "vision": vision,
            "visionStatus": s["vision"],
            "people": s["people"],
            "nearest": s["nearest_distance"],
            "arduino": "online" if s["serial"] == "connected" else "offline",
            "arduinoPort": "",
            "firmware": fw,
            "manualCapable": "3." in fw,
            "line": s.get("line", ""),
            "lineLost": bool(s.get("line_lost")),
            "maxPwm": 180,
            "slowPwm": 180,
            "speed": s.get("speed", 40),
            "fps": round(s.get("fps") or 0, 1),
            "events": s.get("events", []),
            "version": VERSION,
            "online": True,
            "updatedAt": {".sv": "timestamp"},
        }

    def publisher(self) -> None:
        while not self._stop.is_set():
            self._kick.wait(self.publish_s)
            self._kick.clear()
            self.patch("", {"state": self.state()})

    # ── local changes -> request (so the console and the Pi agree) ─────
    def on_dashboard_change(self, what: str, who: str) -> None:
        if self.applying or not self.enabled:
            return
        mode = MODE_TO_FB.get(self.dash.snapshot()["mode"], "STOP")
        rid = f"pi-{int(time.time() * 1000)}"
        self.last_request_id = rid
        threading.Thread(target=self.patch, args=("", {"request": {
            "mode": mode, "by": "pi", "name": who, "id": rid, "at": {".sv": "timestamp"}}}), daemon=True).start()
        self._kick.set()

    # ── stream in ──────────────────────────────────────────────────────
    def handle(self, path: str, data, snapshot: bool) -> None:
        if path == "/":
            data = data or {}
            if snapshot:
                req = data.get("request")
                if isinstance(req, dict):
                    self.last_request_id = req.get("id")   # don't replay an old request at start-up
                return
            for key in ("request", "drive"):
                if key in data:
                    self.handle("/" + key, data[key], False)
            return
        if path.startswith("/state"):
            return
        if path.startswith("/request"):
            req = data if path == "/request" else self.get("/request")
            if not isinstance(req, dict) or (req.get("id") and req.get("id") == self.last_request_id):
                return
            self.last_request_id = req.get("id")
            who = str(req.get("name") or "console")[:40]
            self.applying = True
            try:
                ok, msg = self.dash.remote_mode(str(req.get("mode", "")), who)
            finally:
                self.applying = False
            if not ok:
                print(f"[FIREBASE] request refused: {msg}", flush=True)
            self._kick.set()
            return
        if path.startswith("/drive") and not snapshot:
            d = data if path == "/drive" else self.get("/drive")
            if isinstance(d, dict):
                self.dash.remote_drive(d.get("l", 0) or 0, d.get("r", 0) or 0, str(d.get("name") or "console")[:40])

    def stream(self) -> None:
        backoff = 2
        while not self._stop.is_set():
            try:
                with self.http.get(f"{self.base}.json", params={"auth": self.token()},
                                   headers={"Accept": "text/event-stream"}, stream=True, timeout=(10, 75)) as r:
                    if r.status_code != 200:
                        raise RuntimeError(f"stream HTTP {r.status_code}")
                    if not self.online:
                        print("[FIREBASE] connected: the Vercel console can drive", flush=True)
                    self.online, backoff, first, event = True, 2, True, None
                    for raw in r.iter_lines(decode_unicode=True):
                        if self._stop.is_set():
                            return
                        if not raw:
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
                                raise RuntimeError(f"stream cancelled: {payload}")
                        if time.time() > self.expires - 240:
                            break
            except Exception as exc:  # noqa: BLE001
                if self.online:
                    print(f"[FIREBASE] disconnected ({exc})", flush=True)
                elif not self._stop.is_set():
                    print(f"[FIREBASE] {exc}", flush=True)
                self.online = False
                self._stop.wait(backoff)
                backoff = min(backoff * 2, 60)

    def start(self) -> bool:
        if not self.enabled:
            print("[FIREBASE] not configured (control.env): Vercel Drive page off; local dashboard + SSH still work",
                  flush=True)
            return False
        self.dash.on_change.append(self.on_dashboard_change)
        threading.Thread(target=self.stream, name="fb-stream", daemon=True).start()
        threading.Thread(target=self.publisher, name="fb-publish", daemon=True).start()
        # tell the console the mode the robot actually starts in
        self.on_dashboard_change("start", "start-up")
        return True

    def stop(self) -> None:
        self._stop.set()
        self._kick.set()
