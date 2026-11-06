#!/usr/bin/env python3
"""
MediCart — Raspberry Pi Wi-Fi setup portal (NetworkManager)
IUB Robotics Team

At boot: if wlan0 is not on a Wi-Fi network after a grace period, the Pi
opens a hotspot ("MediCart-Cart-Setup"). Join it from a phone, the setup
page opens by itself (captive portal), pick the network, enter the admin
password, done. The Pi joins the network and remembers it.

Needs Raspberry Pi OS Bookworm or newer (NetworkManager). Runs as root via
systemd (medicart-wifi-portal.service). Only the Python standard library.

    sudo medicart-wifi-setup            # force the portal now (e.g. over SSH)
    python3 medicart-wifi-portal.py --dry-run --port 8080   # try the page on a laptop
"""
from __future__ import annotations

import argparse
import hmac
import html
import logging
import os
import socket
import subprocess
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CONF_PATH = "/etc/medicart/portal.conf"
HOTSPOT_CON = "MediCart-Setup"
AP_IP = "10.42.0.1"

DEFAULTS = {
    "ADMIN_PASSWORD": "medicart-admin",
    "AP_SSID": "MediCart-Cart-Setup",
    "AP_PASSWORD": "medicart-setup",
    "IFACE": "wlan0",
    "BOOT_GRACE": "45",       # seconds to wait for a saved network at boot
    "PORTAL_WINDOW": "600",   # seconds the hotspot stays up before retrying saved networks
}

log = logging.getLogger("medicart-portal")


# ─── Config ──────────────────────────────────────────────────────
def load_config(path: str) -> dict:
    cfg = dict(DEFAULTS)
    try:
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                cfg[k.strip()] = v.strip().strip('"').strip("'")
    except FileNotFoundError:
        log.warning("%s not found — using defaults (change the admin password!)", path)
    if len(cfg["AP_PASSWORD"]) < 8:
        log.warning("AP_PASSWORD shorter than 8 characters — using default")
        cfg["AP_PASSWORD"] = DEFAULTS["AP_PASSWORD"]
    return cfg


# ─── NetworkManager wrapper ──────────────────────────────────────
def split_terse(line: str) -> list[str]:
    """Split one `nmcli -t` line on unescaped ':' and unescape '\\:' / '\\\\'."""
    out, cur, esc = [], [], False
    for ch in line:
        if esc:
            cur.append(ch)
            esc = False
        elif ch == "\\":
            esc = True
        elif ch == ":":
            out.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    out.append("".join(cur))
    return out


class Net:
    def __init__(self, iface: str, dry_run: bool = False):
        self.iface = iface
        self.dry = dry_run
        self._dry_connected = False

    def _run(self, *args: str, timeout: int = 40) -> subprocess.CompletedProcess:
        cmd = ["nmcli", *args]
        shown = [("****" if prev in ("password", "wifi-sec.psk") else a) for prev, a in zip([""] + cmd, cmd)]
        log.debug("$ %s", " ".join(shown))
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)

    def wifi_connected(self) -> str | None:
        """Return the active Wi-Fi connection name on IFACE, ignoring our hotspot."""
        if self.dry:
            return "Demo-Network" if self._dry_connected else None
        try:
            r = self._run("-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device", timeout=10)
        except (subprocess.TimeoutExpired, FileNotFoundError):
            return None
        for line in r.stdout.splitlines():
            parts = split_terse(line)
            if len(parts) >= 4 and parts[0] == self.iface and parts[1] == "wifi":
                if parts[2] == "connected" and parts[3] and parts[3] != HOTSPOT_CON:
                    return parts[3]
        return None

    def scan(self) -> list[dict]:
        if self.dry:
            return [
                {"ssid": "IUB-Robotics", "signal": 82, "secure": True},
                {"ssid": "IUB-Students", "signal": 64, "secure": True},
                {"ssid": "Guest <open>", "signal": 40, "secure": False},
            ]
        try:
            r = self._run("-t", "-f", "SSID,SIGNAL,SECURITY", "device", "wifi", "list",
                          "ifname", self.iface, "--rescan", "yes", timeout=30)
        except subprocess.TimeoutExpired:
            return []
        best: dict[str, dict] = {}
        for line in r.stdout.splitlines():
            parts = split_terse(line)
            if len(parts) < 3 or not parts[0]:
                continue
            ssid, sig, sec = parts[0], parts[1], parts[2]
            try:
                signal = int(sig)
            except ValueError:
                signal = 0
            secure = bool(sec and sec != "--")
            if ssid not in best or signal > best[ssid]["signal"]:
                best[ssid] = {"ssid": ssid, "signal": signal, "secure": secure}
        return sorted(best.values(), key=lambda n: -n["signal"])[:30]

    def start_hotspot(self, ssid: str, psk: str) -> bool:
        if self.dry:
            log.info("[dry-run] hotspot %s up", ssid)
            return True
        self._run("connection", "delete", "id", HOTSPOT_CON, timeout=15)
        add = self._run(
            "connection", "add", "type", "wifi", "ifname", self.iface, "con-name", HOTSPOT_CON,
            "autoconnect", "no", "ssid", ssid,
            "802-11-wireless.mode", "ap", "802-11-wireless.band", "bg",
            "ipv4.method", "shared", "ipv4.addresses", f"{AP_IP}/24", "ipv6.method", "disabled",
            "wifi-sec.key-mgmt", "wpa-psk", "wifi-sec.psk", psk,
        )
        if add.returncode != 0:
            log.error("could not create hotspot: %s", add.stderr.strip())
            return False
        up = self._run("connection", "up", HOTSPOT_CON, timeout=30)
        if up.returncode != 0:
            log.error("could not start hotspot: %s", up.stderr.strip())
            return False
        log.info("hotspot %s up at %s", ssid, AP_IP)
        return True

    def stop_hotspot(self) -> None:
        if self.dry:
            log.info("[dry-run] hotspot down")
            return
        self._run("connection", "down", HOTSPOT_CON, timeout=20)

    def connection_exists(self, name: str) -> bool:
        if self.dry:
            return False
        r = self._run("-t", "-f", "NAME", "connection", "show", timeout=10)
        return any(split_terse(l)[0] == name for l in r.stdout.splitlines())

    def connect(self, ssid: str, password: str, hidden: bool) -> tuple[bool, str]:
        if self.dry:
            time.sleep(2)
            ok = password != "wrong"
            self._dry_connected = ok
            return ok, "" if ok else "Secrets were required, but not provided."
        existed = self.connection_exists(ssid)
        self._run("device", "wifi", "rescan", "ifname", self.iface, timeout=15)
        time.sleep(4)
        args = ["--wait", "40", "device", "wifi", "connect", ssid]
        if password:
            args += ["password", password]
        args += ["ifname", self.iface]
        if hidden:
            args += ["hidden", "yes"]
        try:
            r = self._run(*args, timeout=60)
        except subprocess.TimeoutExpired:
            r = subprocess.CompletedProcess(args, 1, "", "timed out")
        if r.returncode == 0:
            self._run("connection", "modify", "id", ssid, "connection.autoconnect", "yes",
                      "connection.autoconnect-priority", "10", timeout=10)
            return True, ""
        if not existed:  # don't leave a broken profile behind
            self._run("connection", "delete", "id", ssid, timeout=10)
        return False, (r.stderr or r.stdout).strip().splitlines()[-1] if (r.stderr or r.stdout).strip() else "unknown error"


# ─── Portal state ────────────────────────────────────────────────
class Portal:
    def __init__(self, cfg: dict, net: Net):
        self.cfg = cfg
        self.net = net
        self.networks: list[dict] = []
        self.last_error = ""
        self.busy = False
        self.done = threading.Event()
        self.lock = threading.Lock()

    def check_admin(self, given: str) -> bool:
        return hmac.compare_digest(given.encode(), self.cfg["ADMIN_PASSWORD"].encode())

    def attempt(self, ssid: str, password: str, hidden: bool) -> None:
        with self.lock:
            self.busy = True
        time.sleep(2)  # let the "connecting" page reach the phone first
        self.net.stop_hotspot()
        ok, err = self.net.connect(ssid, password, hidden)
        if ok:
            log.info("joined %s", ssid)
            self.last_error = ""
            self.done.set()
        else:
            log.warning("could not join %s: %s", ssid, err)
            self.last_error = f"Could not join “{ssid}”: {err}. Check the password and try again."
            self.networks = self.net.scan() or self.networks
            self.net.start_hotspot(self.cfg["AP_SSID"], self.cfg["AP_PASSWORD"])
        with self.lock:
            self.busy = False


# ─── HTML ────────────────────────────────────────────────────────
CSS = """
:root{--n:#0A2647;--g:#C8861B;--p:#5B2C8E;--bg:#F2F5F9;--t:#0E1A2B;--m:#75859A;--l:#D6DEE9}
*{box-sizing:border-box}body{margin:0;font:15px/1.5 system-ui,-apple-system,'Segoe UI',sans-serif;background:var(--bg);color:var(--t)}
header{background:var(--n);color:#fff;padding:22px 18px 26px}header small{color:#E6A537;font-weight:700;letter-spacing:.14em;font-size:11px}
header h1{margin:4px 0 0;font:600 24px Georgia,serif}main{max-width:480px;margin:-14px auto 24px;padding:0 14px}
.c{background:#fff;border-radius:14px;padding:18px;box-shadow:0 6px 20px -8px rgba(10,38,71,.25);margin-bottom:14px}
label{display:block;font-size:12px;font-weight:600;color:#45556A;margin:12px 0 5px}
input,select{width:100%;height:44px;border:1px solid var(--l);border-radius:10px;padding:0 12px;font:inherit;background:#fff;color:inherit}
input:focus,select:focus{outline:none;border-color:#1B5292;box-shadow:0 0 0 3px rgba(27,82,146,.18)}
button{width:100%;height:48px;border:0;border-radius:12px;background:var(--n);color:#fff;font:inherit;font-weight:700;margin-top:18px;cursor:pointer}
.chk{display:flex;gap:8px;align-items:center;margin-top:12px;font-size:13px;color:#45556A}.chk input{width:auto;height:auto}
.err{background:#FCE5E6;color:#C41E2A;border-radius:10px;padding:10px 12px;font-size:13px;margin-bottom:6px}
.ok{border-left:5px solid #15803D}.m{color:var(--m);font-size:12px}
dl{display:grid;grid-template-columns:auto 1fr;gap:4px 14px;margin:0;font-size:13px}dt{color:var(--m)}dd{margin:0;font-weight:600;text-align:right}
"""


def page(body: str, title: str = "MediCart Cart Setup") -> bytes:
    return (
        "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        f"<title>{html.escape(title)}</title><style>{CSS}</style></head><body>"
        "<header><small>IUB ROBOTICS TEAM</small><h1>MediCart Cart Setup</h1></header>"
        f"<main>{body}</main></body></html>"
    ).encode("utf-8")


def form_page(portal: Portal) -> bytes:
    host = socket.gethostname()
    opts = "".join(
        f"<option value='{html.escape(n['ssid'], quote=True)}'>{html.escape(n['ssid'])}"
        f" — {n['signal']}%{'' if n['secure'] else ' (open)'}</option>"
        for n in portal.networks
    )
    err = f"<div class='err'>{html.escape(portal.last_error)}</div>" if portal.last_error else ""
    body = f"""
<div class='c'><dl><dt>Device</dt><dd>{html.escape(host)}</dd><dt>Interface</dt><dd>{html.escape(portal.net.iface)}</dd>
<dt>Networks found</dt><dd>{len(portal.networks)}</dd></dl></div>
<form class='c' method='POST' action='/connect' autocomplete='off'>
{err}
<label>Admin password</label><input name='admin' type='password' required>
<label>Wi-Fi network</label>
<select onchange="if(this.value)document.getElementById('ssid').value=this.value">
<option value=''>{'Choose a network…' if portal.networks else 'No networks found — type the name below'}</option>{opts}</select>
<label>Network name (SSID)</label><input id='ssid' name='ssid' required maxlength='32'>
<label>Wi-Fi password</label><input name='password' type='password' maxlength='64' placeholder='Leave empty for open networks'>
<label class='chk'><input type='checkbox' name='hidden' value='1'> This is a hidden network</label>
<button>Connect MediCart</button>
<p class='m'>The setup hotspot closes while the cart connects. If it cannot join, the hotspot
returns in about a minute with the error shown here.</p>
</form>"""
    return page(body)


class Handler(BaseHTTPRequestHandler):
    portal: Portal = None  # set at startup
    server_version = "MediCartPortal/2.0"

    def log_message(self, fmt, *args):  # route to logging
        log.debug("%s - %s", self.address_string(), fmt % args)

    def _send(self, code: int, body: bytes, ctype: str = "text/html; charset=utf-8") -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _redirect(self) -> None:
        self.send_response(302)
        self.send_header("Location", f"http://{AP_IP}/")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        host = (self.headers.get("Host") or "").split(":")[0]
        if path == "/" and host in (AP_IP, "localhost", "127.0.0.1", "") or (path == "/" and self.portal.net.dry):
            if self.portal.busy:
                self._send(200, page("<div class='c ok'><h2>Connecting…</h2><p>Please wait.</p></div>"))
            else:
                self._send(200, form_page(self.portal))
        else:
            # OS connectivity checks and any other host → captive page
            self._redirect()

    def do_POST(self):
        if urllib.parse.urlparse(self.path).path != "/connect":
            self._redirect()
            return
        length = min(int(self.headers.get("Content-Length") or 0), 8192)
        form = urllib.parse.parse_qs(self.rfile.read(length).decode("utf-8", "replace"))
        get = lambda k: (form.get(k) or [""])[0]
        if self.portal.busy:
            self._send(409, page("<div class='c'><h2>Already connecting</h2><p>Please wait a moment.</p></div>"))
            return
        if not self.portal.check_admin(get("admin")):
            self.portal.last_error = "Wrong admin password — nothing was changed."
            self._send(403, form_page(self.portal))
            return
        ssid = get("ssid").strip()
        password = get("password")
        if not ssid or len(ssid) > 32:
            self.portal.last_error = "Enter the Wi-Fi network name."
            self._send(400, form_page(self.portal))
            return
        if password and len(password) < 8:
            self.portal.last_error = "Wi-Fi passwords (WPA2) have at least 8 characters."
            self._send(400, form_page(self.portal))
            return
        self.portal.last_error = ""
        body = (f"<div class='c ok'><h2>Connecting to {html.escape(ssid)}…</h2>"
                "<p>The setup hotspot will now close. Reconnect your phone to your normal Wi-Fi.</p>"
                f"<p class='m'>If MediCart cannot join, <b>{html.escape(self.portal.cfg['AP_SSID'])}</b> "
                "comes back in about a minute — join it again to see what went wrong.</p></div>")
        self._send(200, page(body))
        threading.Thread(target=self.portal.attempt, args=(ssid, password, get("hidden") == "1"), daemon=True).start()


# ─── Main loop ───────────────────────────────────────────────────
def wait_for_wifi(net: Net, seconds: int) -> str | None:
    deadline = time.time() + seconds
    while time.time() < deadline:
        name = net.wifi_connected()
        if name:
            return name
        time.sleep(3)
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description="MediCart Raspberry Pi Wi-Fi setup portal")
    ap.add_argument("--config", default=CONF_PATH)
    ap.add_argument("--force", action="store_true", help="open the portal even if Wi-Fi is connected")
    ap.add_argument("--dry-run", action="store_true", help="no nmcli calls; for testing the page on any computer")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    cfg = load_config(args.config)
    net = Net(cfg["IFACE"], dry_run=args.dry_run)

    if not args.dry_run and os.geteuid() != 0:
        log.error("run as root (sudo) — NetworkManager changes need it")
        return 1

    if not args.force:
        name = wait_for_wifi(net, int(cfg["BOOT_GRACE"]))
        if name:
            log.info("Wi-Fi connected (%s) — portal not needed", name)
            return 0

    portal = Portal(cfg, net)
    Handler.portal = portal
    httpd = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    log.info("portal listening on port %d", args.port)

    window = int(cfg["PORTAL_WINDOW"])
    try:
        while True:
            portal.networks = net.scan()  # scan before the radio switches to AP mode
            log.info("found %d networks", len(portal.networks))
            if not net.start_hotspot(cfg["AP_SSID"], cfg["AP_PASSWORD"]):
                time.sleep(20)
                continue
            log.info("join '%s' (password '%s') and open http://%s", cfg["AP_SSID"], cfg["AP_PASSWORD"], AP_IP)
            if portal.done.wait(timeout=window):
                break
            if portal.busy:
                portal.done.wait(timeout=120)
                if portal.done.is_set():
                    break
            log.info("no setup within %d s — retrying saved networks", window)
            net.stop_hotspot()
            if wait_for_wifi(net, 60):
                break
    except KeyboardInterrupt:
        net.stop_hotspot()
        return 130
    finally:
        time.sleep(1)
        httpd.shutdown()
    log.info("Wi-Fi ready: %s", net.wifi_connected())
    return 0


if __name__ == "__main__":
    sys.exit(main())
