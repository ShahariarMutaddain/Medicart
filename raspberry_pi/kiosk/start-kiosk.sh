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
