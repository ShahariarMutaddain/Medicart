#!/usr/bin/env bash
# ════════════════════════════════════════════════════════════════
#  MediCart — plug and play on Ubuntu Desktop (Raspberry Pi 5)
#
#  Power on → logs in by itself → cart screen full screen → robot program,
#  camera splitter, Pi dashboard and Vercel Drive link all start by themselves.
#
#  Run once, after install.sh:
#     sudo bash ubuntu-autostart.sh robos141          (the user shown on the Pi's screen)
#  Safe to run again. install.sh runs it by itself on Ubuntu.
# ════════════════════════════════════════════════════════════════
set -u
G=$'\033[32m'; Y=$'\033[33m'; R=$'\033[31m'; N=$'\033[0m'
ok()   { echo "${G}✓${N} $*"; }
warn() { echo "${Y}!${N} $*"; }
die()  { echo "${R}✗${N} $*"; exit 1; }

[ "$(id -u)" = 0 ] || die "Run with sudo:  sudo bash $0 <screen-user>"
U="${1:-${KIOSK_USER:-${SUDO_USER:-}}}"
[ -n "$U" ] || die "Give the screen user:  sudo bash $0 robos141"
H="$(getent passwd "$U" | cut -d: -f6)"
[ -n "$H" ] || die "User $U not found"
[ -f /etc/gdm3/custom.conf ] || die "This is for Ubuntu Desktop (GNOME / gdm3)"
echo "  MediCart plug and play · screen user: $U"
as_user() { sudo -u "$U" -H "$@"; }
gset() { as_user dbus-run-session gsettings set "$@" 2>/dev/null || warn "gsettings $*"; }

# ─── 1. Log in by itself, X11 (screen sharing + kiosk are reliable there) ─────
python3 - "$U" <<'EOF'
import re, sys
user, path = sys.argv[1], "/etc/gdm3/custom.conf"
text = open(path).read()
want = {"AutomaticLoginEnable": "True", "AutomaticLogin": user, "WaylandEnable": "false"}
lines, out, in_daemon, seen = text.splitlines(), [], False, set()
for line in lines:
    s = line.strip()
    if s.startswith("["):
        if in_daemon:
            out += [f"{k}={v}" for k, v in want.items() if k not in seen]; seen |= set(want)
        in_daemon = s == "[daemon]"
    m = re.match(r"#?\s*(AutomaticLoginEnable|AutomaticLogin|WaylandEnable)\s*=", line)
    if in_daemon and m:
        k = m.group(1)
        if k not in seen:
            out.append(f"{k}={want[k]}"); seen.add(k)
        continue
    out.append(line)
if in_daemon:
    out += [f"{k}={v}" for k, v in want.items() if k not in seen]; seen |= set(want)
if not seen:
    out = ["[daemon]"] + [f"{k}={v}" for k, v in want.items()] + [""] + out
open(path, "w").write("\n".join(out) + "\n")
EOF
ok "Auto-login as $U (X11)"

# ─── 2. Screen never locks, sleeps or shows pop-ups ──────────────
gset org.gnome.desktop.session idle-delay 0
gset org.gnome.desktop.screensaver lock-enabled false
gset org.gnome.desktop.screensaver idle-activation-enabled false
gset org.gnome.desktop.screensaver ubuntu-lock-on-suspend false
gset org.gnome.desktop.lockdown disable-lock-screen true
gset org.gnome.settings-daemon.plugins.power idle-dim false
gset org.gnome.settings-daemon.plugins.power sleep-inactive-ac-type "'nothing'"
gset org.gnome.desktop.notifications show-banners false
install -d -o "$U" -g "$U" "$H/.config" "$H/.config/autostart"
echo yes > "$H/.config/gnome-initial-setup-done"; chown "$U:$U" "$H/.config/gnome-initial-setup-done"
for app in update-notifier gnome-initial-setup-first-login org.gnome.Software ubuntu-advantage-notification; do
  printf '[Desktop Entry]\nType=Application\nName=%s\nHidden=true\n' "$app" > "$H/.config/autostart/$app.desktop"
  chown "$U:$U" "$H/.config/autostart/$app.desktop"
done
if [ -f /etc/default/apport ]; then
  sed -i 's/^enabled=1/enabled=0/' /etc/default/apport
  systemctl disable --now apport.service >/dev/null 2>&1 || true
fi
snap set system refresh.timer=03:00-04:00 >/dev/null 2>&1 || true   # app updates only at night
ok "Screen stays on, no lock screen, no update / crash pop-ups"

# ─── 3. Chromium (snap on Ubuntu) with camera + microphone ───────
if [ ! -x /snap/bin/chromium ] && ! command -v chromium >/dev/null && ! command -v chromium-browser >/dev/null; then
  snap install chromium >/dev/null || die "Could not install Chromium (snap install chromium)"
fi
if [ -x /snap/bin/chromium ]; then
  for plug in camera audio-record audio-playback; do snap connect "chromium:$plug" >/dev/null 2>&1 || true; done
fi
ok "Chromium ready (camera + microphone allowed)"

# ─── 4. Cart screen launcher ─────────────────────────────────────
install -d -m 755 /opt/medicart/kiosk
cat > /opt/medicart/kiosk/start-kiosk.sh <<'KIOSK'
#!/usr/bin/env bash
# MediCart — cart screen launcher (Chromium kiosk). Started at desktop login; relaunches Chromium if it closes.
# Stop it for maintenance:  pkill -f start-kiosk.sh; pkill -f chromium
set -u
CONF=/etc/medicart/kiosk.conf
# shellcheck source=/dev/null
[ -r "$CONF" ] && . "$CONF"
URL="${KIOSK_URL:-}"
LOG="${XDG_RUNTIME_DIR:-/tmp}/medicart-kiosk.log"
exec 9>"${XDG_RUNTIME_DIR:-/tmp}/medicart-kiosk.lock"
flock -n 9 || exit 0                                    # one instance only
[ -n "$URL" ] || { echo "KIOSK_URL is not set in $CONF" >>"$LOG"; exit 1; }

if [ -x /snap/bin/chromium ]; then
  BROWSER=/snap/bin/chromium
  PROFILE="${KIOSK_PROFILE:-$HOME/snap/chromium/common/medicart-kiosk}"   # a snap can't write ~/.config
else
  BROWSER="$(command -v chromium-browser || command -v chromium || true)"
  PROFILE="${KIOSK_PROFILE:-$HOME/.config/medicart-kiosk}"
fi
[ -n "$BROWSER" ] || { echo "Chromium not installed" >>"$LOG"; exit 1; }

if command -v xset >/dev/null 2>&1 && [ -n "${DISPLAY:-}" ]; then
  xset s off -dpms s noblank 2>/dev/null || true       # keep the screen on
fi
for _ in $(seq 1 45); do                               # wait up to 90 s for the internet
  getent hosts www.gstatic.com >/dev/null 2>&1 && break
  sleep 2
done
sleep 3                                                # let the desktop finish starting

mkdir -p "$PROFILE/Default"
while true; do
  sed -i 's/"exited_cleanly":false/"exited_cleanly":true/; s/"exit_type":"[^"]*"/"exit_type":"Normal"/' \
    "$PROFILE/Default/Preferences" 2>/dev/null || true
  echo "$(date '+%F %T') starting $BROWSER" >>"$LOG"
  "$BROWSER" \
    --kiosk "$URL" \
    --user-data-dir="$PROFILE" \
    --noerrdialogs --disable-infobars --no-first-run --no-default-browser-check \
    --disable-session-crashed-bubble --disable-features=Translate,TranslateUI \
    --check-for-update-interval=31536000 \
    --use-fake-ui-for-media-stream \
    --autoplay-policy=no-user-gesture-required \
    --overscroll-history-navigation=0 \
    --password-store=basic \
    >>"$LOG" 2>&1
  sleep 3
done
KIOSK
chmod 755 /opt/medicart/kiosk/start-kiosk.sh
cat > "$H/.config/autostart/medicart-kiosk.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=MediCart Cart Screen
Comment=IUB Robotics Team — MediCart patient screen
Exec=/opt/medicart/kiosk/start-kiosk.sh
X-GNOME-Autostart-enabled=true
X-GNOME-Autostart-Delay=5
NoDisplay=false
EOF
chown "$U:$U" "$H/.config/autostart/medicart-kiosk.desktop"
[ -r /etc/medicart/kiosk.conf ] && grep -q '^KIOSK_URL=.' /etc/medicart/kiosk.conf \
  || { install -d /etc/medicart; echo "KIOSK_URL=https://medicart-iub.vercel.app/cart" > /etc/medicart/kiosk.conf; }
ok "Cart screen opens full screen at login ($(sed -n 's/^KIOSK_URL=//p' /etc/medicart/kiosk.conf))"

# ─── 5. Shared camera (YOLO + video call) ────────────────────────
if ! modinfo v4l2loopback >/dev/null 2>&1; then
  warn "Building the v4l2loopback module (a few minutes)…"
  apt-get install -y -qq "linux-headers-$(uname -r)" >/dev/null 2>&1 || apt-get install -y -qq linux-headers-raspi >/dev/null 2>&1 || true
  apt-get install -y -qq v4l2loopback-dkms >/dev/null 2>&1 || warn "v4l2loopback-dkms failed: sudo apt full-upgrade, reboot, run this again"
fi
if [ ! -f /etc/modprobe.d/medicart-v4l2loopback.conf ]; then
  echo "options v4l2loopback devices=2 video_nr=10,11 card_label=MediCart-Vision,MediCart-Call exclusive_caps=1,1 max_buffers=2" \
    > /etc/modprobe.d/medicart-v4l2loopback.conf
fi
echo v4l2loopback > /etc/modules-load.d/medicart-v4l2loopback.conf
usermod -aG video,audio,dialout "$U" || true
ok "Camera split: /dev/video10 → YOLO, /dev/video11 → video call"

# ─── 6. Everything starts at power-on ────────────────────────────
systemctl daemon-reload
for s in medicart-camera medicart-vision medicart-wifi-portal; do
  if [ -f "/etc/systemd/system/$s.service" ]; then
    systemctl enable "$s.service" >/dev/null 2>&1 && ok "$s starts at power-on"
  else
    warn "$s.service is not installed: run install.sh first"
  fi
done

# ─── 7. One command to check everything ──────────────────────────
cat > /usr/local/bin/medicart-status <<'EOF'
#!/usr/bin/env bash
# MediCart: is everything running?
G=$'\033[32m'; R=$'\033[31m'; N=$'\033[0m'
chk() { if eval "$2" >/dev/null 2>&1; then echo "${G}✓${N} $1"; else echo "${R}✗${N} $1"; fi; }
chk "Camera split (video10 + video11)" "[ -e /dev/video10 ] && [ -e /dev/video11 ] && systemctl is-active -q medicart-camera"
chk "Robot program (medicart-vision)" "systemctl is-active -q medicart-vision"
chk "Arduino connected"               "ls /dev/ttyACM* /dev/ttyUSB*"
chk "Cart screen (Chromium kiosk)"    "pgrep -f 'chromium.*--kiosk'"
chk "Internet"                        "getent hosts www.gstatic.com"
echo "── last robot program messages:"
sudo journalctl -u medicart-vision -n 300 --no-pager 2>/dev/null | grep -E "\[(WEB|ARDUINO|FIREBASE)\]" | tail -5
EOF
chmod 755 /usr/local/bin/medicart-status
ok "Check any time with:  medicart-status"

echo
echo "${G}Done.${N} Reboot now:  sudo reboot"
echo "First boot only: sign in on the cart screen with cart01@medicart.iub (it stays signed in after that)."
