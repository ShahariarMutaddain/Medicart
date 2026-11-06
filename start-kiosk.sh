#!/usr/bin/env bash
# ════════════════════════════════════════════════════════════════
#  MediCart — cart screen launcher (Chromium kiosk)
#  Started at desktop login. Relaunches Chromium if it closes.
#  Stop it for maintenance:  pkill -f start-kiosk.sh; pkill chromium
# ════════════════════════════════════════════════════════════════
set -u

CONF=/etc/medicart/kiosk.conf
# shellcheck source=/dev/null
[ -r "$CONF" ] && . "$CONF"
URL="${KIOSK_URL:-}"
PROFILE="${KIOSK_PROFILE:-$HOME/.config/medicart-kiosk}"

# one instance only (XDG autostart and labwc autostart may both fire)
exec 9>"${XDG_RUNTIME_DIR:-/tmp}/medicart-kiosk.lock"
flock -n 9 || exit 0

if [ -z "$URL" ]; then
  echo "KIOSK_URL is not set in $CONF" >&2
  exit 1
fi

BROWSER="$(command -v chromium-browser || command -v chromium || true)"
if [ -z "$BROWSER" ]; then
  echo "Chromium not installed (sudo apt install chromium-browser)" >&2
  exit 1
fi

# keep the screen on (X11 sessions; Wayland uses raspi-config blanking setting)
if command -v xset >/dev/null 2>&1 && [ -n "${DISPLAY:-}" ]; then
  xset s off -dpms s noblank 2>/dev/null || true
fi

# wait up to 90 s for the internet — the page also copes with being offline
for _ in $(seq 1 45); do
  getent hosts www.gstatic.com >/dev/null 2>&1 && break
  sleep 2
done

mkdir -p "$PROFILE/Default"
while true; do
  # suppress "Chromium didn't shut down correctly" after power cuts
  sed -i 's/"exited_cleanly":false/"exited_cleanly":true/; s/"exit_type":"[^"]*"/"exit_type":"Normal"/' \
    "$PROFILE/Default/Preferences" 2>/dev/null || true

  "$BROWSER" \
    --kiosk "$URL" \
    --user-data-dir="$PROFILE" \
    --noerrdialogs --disable-infobars --no-first-run \
    --disable-session-crashed-bubble --disable-features=Translate,TranslateUI \
    --check-for-update-interval=31536000 \
    --use-fake-ui-for-media-stream \
    --autoplay-policy=no-user-gesture-required \
    --overscroll-history-navigation=0 \
    --password-store=basic \
    >/dev/null 2>&1

  sleep 3
done
