#!/usr/bin/env bash
# ════════════════════════════════════════════════════════════════
#  MediCart — Raspberry Pi installer
#  IUB Robotics Team
#
#  Installs:
#    • Camera splitter     (one USB camera → "MediCart-Vision" for OpenCV
#                           + "MediCart-Call" for the video call)
#    • Wi-Fi setup portal  (hotspot + captive page when no Wi-Fi)
#    • Cart screen kiosk   (Chromium full-screen → /cart page, autostart)
#    • Vision service      (optional: runs medicart.py headless at boot)
#    • Drive controller    (medicart-control: AUTO / MANUAL / STOP, owns the Arduino port,
#                           web + SSH manual driving; needs Arduino firmware 2.1 for MANUAL)
#
#  Usage:   cd raspberry_pi && sudo ./install.sh
#  Non-interactive:
#    sudo KIOSK_URL=https://x.vercel.app/cart ADMIN_PASSWORD=secret \
#         VISION_SCRIPT=/home/pi/MideCart/medicart.py ./install.sh --yes
# ════════════════════════════════════════════════════════════════
set -euo pipefail

YES=0
[ "${1:-}" = "--yes" ] && YES=1

say()  { printf '\033[1;34m▸\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m✓\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m✗\033[0m %s\n' "$*" >&2; exit 1; }
ask_secret() { # ask_secret VAR "Prompt" — hidden input, keeps an existing value
  local var="$1" prompt="$2" cur="${!1:-}"
  if [ -n "$cur" ]; then return; fi
  if [ "$YES" = 1 ]; then return; fi
  local reply
  read -r -s -p "$prompt: " reply; echo
  printf -v "$var" '%s' "$reply"
}
ask()  { # ask VAR "Prompt" "default"
  local var="$1" prompt="$2" def="${3:-}" cur="${!1:-}"
  if [ -n "$cur" ]; then return; fi
  if [ "$YES" = 1 ]; then printf -v "$var" '%s' "$def"; return; fi
  local reply
  read -r -p "$prompt${def:+ [$def]}: " reply
  printf -v "$var" '%s' "${reply:-$def}"
}

[ "$(id -u)" -eq 0 ] || die "Run with sudo:  sudo ./install.sh"
SRC="$(cd "$(dirname "$0")" && pwd)"
TARGET_USER="${SUDO_USER:-${TARGET_USER:-pi}}"
TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
[ -n "$TARGET_HOME" ] || die "User $TARGET_USER not found"

command -v nmcli >/dev/null || die "NetworkManager (nmcli) not found. Use Raspberry Pi OS Bookworm or newer."
systemctl is-active --quiet NetworkManager || die "NetworkManager is not running. Enable it: sudo raspi-config → Advanced → Network Config → NetworkManager"

echo
printf '\033[1m  MediCart · Raspberry Pi setup\033[0m  (user: %s)\n\n' "$TARGET_USER"

# ─── Questions ───────────────────────────────────────────────────
EXISTING_URL="$(sed -n 's/^KIOSK_URL=//p' /etc/medicart/kiosk.conf 2>/dev/null || true)"
ask KIOSK_URL "Cart page URL (your Vercel site + /cart)" "${EXISTING_URL:-https://YOUR-PROJECT.vercel.app/cart}"
case "$KIOSK_URL" in https://*) ;; *) die "The cart page must be https:// (camera access needs it)";; esac

if [ -f /etc/medicart/portal.conf ]; then
  ADMIN_PASSWORD="${ADMIN_PASSWORD:-$(sed -n 's/^ADMIN_PASSWORD=//p' /etc/medicart/portal.conf)}"
fi
ask ADMIN_PASSWORD "Admin password for the Wi-Fi setup page" "medicart-admin"
ask AP_PASSWORD "Password of the setup hotspot (8+ chars)" "medicart-setup"
[ "${#AP_PASSWORD}" -ge 8 ] || die "Hotspot password needs 8+ characters"

EXISTING_VS="$(sed -n 's/^VISION_SCRIPT=//p' /etc/medicart/medicart.env 2>/dev/null || true)"
ask VISION_SCRIPT "Path to medicart.py to run at boot (blank = skip)" "$EXISTING_VS"

# drive controller (manual + autonomous)
cget() { sed -n "s/^$1=//p" /etc/medicart/control.env 2>/dev/null | head -1; }
INSTALL_CONTROL="${INSTALL_CONTROL:-}"
ask INSTALL_CONTROL "Install the drive controller (manual + auto driving)? y/n" "y"
case "$INSTALL_CONTROL" in [yY]*) INSTALL_CONTROL=1;; *) INSTALL_CONTROL=0;; esac
if [ "$INSTALL_CONTROL" = 1 ]; then
  CONTROL_EMAIL="${CONTROL_EMAIL:-$(cget CONTROL_EMAIL)}"
  CONTROL_PASSWORD="${CONTROL_PASSWORD:-$(cget CONTROL_PASSWORD)}"
  FIREBASE_API_KEY="${FIREBASE_API_KEY:-$(cget FIREBASE_API_KEY)}"
  FIREBASE_DB_URL="${FIREBASE_DB_URL:-$(cget FIREBASE_DB_URL)}"
  ask CONTROL_EMAIL "Cart account email (Firebase, same as the cart screen)" "cart01@medicart.iub"
  ask_secret CONTROL_PASSWORD "Cart account password (hidden; stays on this Pi)"
  ask FIREBASE_API_KEY "Firebase apiKey" "AIzaSyAjDepmO5m_fiWIy_HPcVnN2uHtMwGPv7M"
  ask FIREBASE_DB_URL "Firebase databaseURL" "https://medicart-e20e0-default-rtdb.asia-southeast1.firebasedatabase.app"
  [ -n "$CONTROL_PASSWORD" ] || warn "No cart password: web driving stays off until you add it to /etc/medicart/control.env"
fi

# ─── Packages ────────────────────────────────────────────────────
say "Installing packages (Chromium, ffmpeg, v4l2loopback)…"
apt-get update -qq || warn "apt update failed — continuing"
if ! command -v chromium-browser >/dev/null && ! command -v chromium >/dev/null; then
  apt-get install -y -qq chromium-browser || apt-get install -y -qq chromium || die "Could not install Chromium"
fi
apt-get install -y -qq python3 ffmpeg v4l-utils >/dev/null || die "Could not install ffmpeg / v4l-utils"
apt-get install -y -qq python3-serial python3-requests >/dev/null || warn "python3-serial / python3-requests failed to install"
if ! modinfo v4l2loopback >/dev/null 2>&1; then
  say "Building the v4l2loopback kernel module (a few minutes)…"
  apt-get install -y -qq "linux-headers-$(uname -r)" >/dev/null 2>&1 \
    || apt-get install -y -qq raspberrypi-kernel-headers >/dev/null 2>&1 \
    || warn "Kernel headers for $(uname -r) not found — run 'sudo apt full-upgrade', reboot, and re-run install.sh"
  apt-get install -y -qq v4l2loopback-dkms >/dev/null || warn "v4l2loopback-dkms failed to install"
fi
ok "Packages ready"

# ─── Files ───────────────────────────────────────────────────────
say "Copying MediCart files to /opt/medicart…"
install -d -m 755 /opt/medicart/wifi-portal /opt/medicart/kiosk /opt/medicart/vision /opt/medicart/camera /etc/medicart
install -m 755 "$SRC/camera/medicart-camera.sh" /opt/medicart/camera/
install -m 755 "$SRC/wifi-portal/medicart-wifi-portal.py" /opt/medicart/wifi-portal/
install -m 755 "$SRC/kiosk/start-kiosk.sh" /opt/medicart/kiosk/
install -m 644 "$SRC/kiosk/medicart-kiosk.desktop" /opt/medicart/kiosk/
install -m 755 "$SRC/vision/run-vision.sh" /opt/medicart/vision/

cat > /etc/medicart/portal.conf <<EOF
# MediCart Wi-Fi portal — written by install.sh
ADMIN_PASSWORD=$ADMIN_PASSWORD
AP_SSID=MediCart-Cart-Setup
AP_PASSWORD=$AP_PASSWORD
IFACE=wlan0
BOOT_GRACE=45
PORTAL_WINDOW=600
EOF
chmod 600 /etc/medicart/portal.conf

cat > /etc/medicart/kiosk.conf <<EOF
# MediCart cart screen — written by install.sh
KIOSK_URL=$KIOSK_URL
EOF
chmod 644 /etc/medicart/kiosk.conf
ok "Config in /etc/medicart"

# ─── Camera splitter ─────────────────────────────────────────────
say "Setting up the shared camera…"
# two virtual cameras: /dev/video10 MediCart-Vision (OpenCV), /dev/video11 MediCart-Call (browser)
# exclusive_caps=1 is what lets Chromium list them as webcams
cat > /etc/modprobe.d/medicart-v4l2loopback.conf <<'EOF'
options v4l2loopback devices=2 video_nr=10,11 card_label=MediCart-Vision,MediCart-Call exclusive_caps=1,1 max_buffers=2
EOF
echo v4l2loopback > /etc/modules-load.d/medicart-v4l2loopback.conf
[ -f /etc/medicart/camera.conf ] || install -m 644 "$SRC/config/camera.conf.example" /etc/medicart/camera.conf
install -m 644 "$SRC/systemd/medicart-camera.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable medicart-camera.service >/dev/null
systemctl stop medicart-camera.service 2>/dev/null || true
modprobe -r v4l2loopback 2>/dev/null || true
if modprobe v4l2loopback 2>/dev/null && [ -e /dev/video10 ] && [ -e /dev/video11 ]; then
  systemctl start medicart-camera.service
  ok "Camera splitter running: /dev/video10 (MediCart-Vision) + /dev/video11 (MediCart-Call)"
else
  warn "v4l2loopback not loaded yet — it loads after the reboot. If not: sudo modprobe v4l2loopback"
fi

# ─── Wi-Fi portal ────────────────────────────────────────────────
say "Installing the Wi-Fi setup portal…"
install -d /etc/NetworkManager/dnsmasq-shared.d
# answer every DNS query with the Pi while the hotspot is up → phones open the setup page
echo "address=/#/10.42.0.1" > /etc/NetworkManager/dnsmasq-shared.d/medicart-captive.conf
install -m 644 "$SRC/systemd/medicart-wifi-portal.service" /etc/systemd/system/
cat > /usr/local/bin/medicart-wifi-setup <<'EOF'
#!/bin/sh
# Open the MediCart Wi-Fi setup hotspot now (even if Wi-Fi works).
exec /usr/bin/python3 /opt/medicart/wifi-portal/medicart-wifi-portal.py --force "$@"
EOF
chmod 755 /usr/local/bin/medicart-wifi-setup
systemctl daemon-reload
systemctl enable medicart-wifi-portal.service >/dev/null
ok "Wi-Fi portal enabled (hotspot: MediCart-Cart-Setup)"

# ─── Kiosk autostart ─────────────────────────────────────────────
say "Setting up the cart screen to start at login…"
AUTOSTART="$TARGET_HOME/.config/autostart"
install -d -o "$TARGET_USER" -g "$TARGET_USER" "$AUTOSTART"
install -m 644 -o "$TARGET_USER" -g "$TARGET_USER" "$SRC/kiosk/medicart-kiosk.desktop" "$AUTOSTART/medicart-kiosk.desktop"
# labwc (Pi OS desktop since late 2024) — belt and braces; the lock prevents a double start
LABWC_DIR="$TARGET_HOME/.config/labwc"
if command -v labwc >/dev/null 2>&1; then
  install -d -o "$TARGET_USER" -g "$TARGET_USER" "$LABWC_DIR"
  touch "$LABWC_DIR/autostart"
  grep -q start-kiosk.sh "$LABWC_DIR/autostart" || echo "/opt/medicart/kiosk/start-kiosk.sh &" >> "$LABWC_DIR/autostart"
  chown "$TARGET_USER:$TARGET_USER" "$LABWC_DIR/autostart"
fi
usermod -aG video,audio "$TARGET_USER" || true
ok "Kiosk autostart installed for $TARGET_USER"

if command -v raspi-config >/dev/null 2>&1; then
  say "Desktop auto-login + no screen blanking…"
  raspi-config nonint do_boot_behaviour B4 || warn "Set Desktop Autologin manually in raspi-config"
  raspi-config nonint do_blanking 1 || warn "Disable screen blanking manually in raspi-config"
  ok "Auto-login to desktop, screen stays on"
fi

# ─── Drive controller ────────────────────────────────────────────
VISION_SERIAL_LINE=""
if [ "$INSTALL_CONTROL" = 1 ]; then
  say "Installing the drive controller (medicart-control)…"
  install -d -m 755 /opt/medicart/control
  install -m 755 "$SRC/control/medicart_control.py" /opt/medicart/control/
  install -m 755 "$SRC/control/medicart-drive" /opt/medicart/control/
  ln -sf /opt/medicart/control/medicart-drive /usr/local/bin/medicart-drive
  umask 077
  cat > /etc/medicart/control.env <<EOF
# MediCart drive controller — written by install.sh (root only). See control.env.example.
CART_ID=medicart-01
FIREBASE_API_KEY=$FIREBASE_API_KEY
FIREBASE_DB_URL=$FIREBASE_DB_URL
CONTROL_EMAIL=$CONTROL_EMAIL
CONTROL_PASSWORD=$CONTROL_PASSWORD
ARDUINO_PORT=auto
VISION_TTY=/run/medicart/vision-tty
START_MODE=${START_MODE:-AUTO}
MANUAL_MAX_PWM=${MANUAL_MAX_PWM:-160}
MANUAL_SLOW_PWM=${MANUAL_SLOW_PWM:-90}
DRIVE_TIMEOUT_S=0.6
EOF
  umask 022
  chmod 600 /etc/medicart/control.env
  sed "s/@USER@/$TARGET_USER/" "$SRC/systemd/medicart-control.service.in" > /etc/systemd/system/medicart-control.service
  usermod -aG dialout "$TARGET_USER" || true
  # ModemManager grabs /dev/ttyACM* for a few seconds at plug-in; MediCart doesn't need it
  systemctl disable --now ModemManager >/dev/null 2>&1 || true
  systemctl daemon-reload
  systemctl enable medicart-control.service >/dev/null
  systemctl restart medicart-control.service || warn "medicart-control did not start — journalctl -u medicart-control"
  VISION_SERIAL_LINE="MEDICART_SERIAL_PORT=/run/medicart/vision-tty"
  ok "Drive controller running — web Drive page + 'medicart-drive' over SSH"
fi

# ─── Vision service ──────────────────────────────────────────────
if [ -n "$VISION_SCRIPT" ]; then
  [ -f "$VISION_SCRIPT" ] || warn "$VISION_SCRIPT does not exist yet — fix /etc/medicart/medicart.env later"
  VDIR="$(dirname "$VISION_SCRIPT")"
  VPY="python3"
  for cand in "$VDIR/venv/bin/python" "$VDIR/../venv/bin/python" "$VDIR/.venv/bin/python" "$TARGET_HOME/venv/bin/python"; do
    [ -x "$cand" ] && VPY="$(readlink -f "$cand")" && break
  done
  ask VISION_PYTHON "Python for medicart.py (venv with ultralytics)" "$VPY"
  VISION_CAMERA="${VISION_CAMERA:-10}"   # /dev/video10 = MediCart-Vision (shared camera)
  cat > /etc/medicart/medicart.env <<EOF
# MediCart vision — written by install.sh
VISION_SCRIPT=$VISION_SCRIPT
VISION_PYTHON=$VISION_PYTHON
MEDICART_VIDEO_SOURCE=$VISION_CAMERA
$VISION_SERIAL_LINE
EOF
  # medicart.py needs the 5-line [CTRL-2.1] port option to share the Arduino with the controller
  if [ -n "$VISION_SERIAL_LINE" ] && [ -f "$VISION_SCRIPT" ] && ! grep -q MEDICART_SERIAL_PORT "$VISION_SCRIPT"; then
    cp -n "$VISION_SCRIPT" "$VISION_SCRIPT.original"
    python3 - "$VISION_SCRIPT" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = 'SERIAL_PORT_CANDIDATES = ("/dev/ttyACM0", "/dev/ttyACM1")\n'
add = ('# [CTRL-2.1] When the medicart-control service owns the Arduino, it gives this script its own\n'
       '# virtual port (MEDICART_SERIAL_PORT=/run/medicart/vision-tty). Not set = original behaviour.\n'
       'if os.environ.get("MEDICART_SERIAL_PORT"):\n'
       '    SERIAL_PORT = os.environ["MEDICART_SERIAL_PORT"]\n'
       '    SERIAL_PORT_CANDIDATES = (SERIAL_PORT,)\n')
if old in s:
    open(p, "w").write(s.replace(old, old + add, 1)); print("patched")
else:
    print("pattern not found")
PY
    chown "$TARGET_USER:$TARGET_USER" "$VISION_SCRIPT" || true
    ok "medicart.py: added the MEDICART_SERIAL_PORT option (backup: $VISION_SCRIPT.original)"
  fi
  sed "s/@USER@/$TARGET_USER/" "$SRC/systemd/medicart-vision.service.in" > /etc/systemd/system/medicart-vision.service
  usermod -aG dialout "$TARGET_USER" || true
  systemctl daemon-reload
  systemctl enable medicart-vision.service >/dev/null
  ok "Vision service enabled (reads the shared camera at /dev/video$VISION_CAMERA)"
else
  warn "Vision service skipped (run medicart.py yourself, or re-run with VISION_SCRIPT=…)"
fi

echo
ok "Done. Reboot to start everything:  sudo reboot"
cat <<EOF

  Shared camera ........... /dev/video10 → medicart.py   /dev/video11 → video call
                            settings: /etc/medicart/camera.conf
  Cart screen URL ......... $KIOSK_URL
  Wi-Fi setup hotspot ..... MediCart-Cart-Setup  (password: $AP_PASSWORD)
  Setup page admin pass ... (the one you just entered)
  Force setup now ......... sudo medicart-wifi-setup
  Logs .................... journalctl -u medicart-camera -f
                            journalctl -u medicart-wifi-portal -f
                            journalctl -u medicart-vision -f
                            journalctl -u medicart-control -f
  Drive controller ........ web console → Drive page,  or over SSH:  medicart-drive
                            needs Arduino firmware 2.1 for MANUAL (AUTO works with any)

  First boot: the cart screen asks for the CART account (Firebase) once.
  It uses the "MediCart-Call" camera automatically. Hold the gear (bottom-right)
  for 1.5 s to check the microphone and speaker.

  If you start medicart.py yourself, point it at the shared camera:
      MEDICART_VIDEO_SOURCE=10 python3 medicart.py
EOF
