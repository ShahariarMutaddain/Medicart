#!/usr/bin/env bash
# ════════════════════════════════════════════════════════════════
#  MediCart — camera splitter
#  One physical USB camera → two virtual cameras (v4l2loopback):
#     /dev/video10  "MediCart-Vision"  → medicart.py (OpenCV / YOLO)
#     /dev/video11  "MediCart-Call"    → cart screen (WebRTC video call)
#  Both programs get the same live picture at the same time.
#  Run by medicart-camera.service (root).
# ════════════════════════════════════════════════════════════════
set -u

CONF=/etc/medicart/camera.conf
# shellcheck source=/dev/null
[ -r "$CONF" ] && . "$CONF"
CAMERA_DEVICE="${CAMERA_DEVICE:-auto}"
CAMERA_FORMAT="${CAMERA_FORMAT:-mjpeg}"
CAMERA_SIZE="${CAMERA_SIZE:-1280x720}"
CAMERA_FPS="${CAMERA_FPS:-30}"
VISION_DEV="${VISION_DEV:-/dev/video10}"
CALL_DEV="${CALL_DEV:-/dev/video11}"

log() { echo "[medicart-camera] $*"; }

# ─── virtual devices ─────────────────────────────────────────────
if [ ! -e "$VISION_DEV" ] || [ ! -e "$CALL_DEV" ]; then
  modprobe v4l2loopback 2>/dev/null || true
  sleep 1
fi
if [ ! -e "$VISION_DEV" ] || [ ! -e "$CALL_DEV" ]; then
  log "virtual cameras $VISION_DEV / $CALL_DEV missing — is v4l2loopback installed? (sudo modprobe v4l2loopback)"
  exit 1
fi

# ─── find the real camera ────────────────────────────────────────
find_camera() {
  local d n
  # stable USB names first (the -video-index0 node is the picture stream)
  for d in /dev/v4l/by-id/*-video-index0; do
    [ -e "$d" ] && { readlink -f "$d"; return 0; }
  done
  # otherwise the first capture node that is not one of our loopbacks
  for n in /sys/class/video4linux/video*; do
    [ -e "$n/name" ] || continue
    grep -q '^MediCart-' "$n/name" && continue
    [ "$(cat "$n/index" 2>/dev/null || echo 0)" = 0 ] || continue
    d="/dev/$(basename "$n")"
    case "$d" in "$VISION_DEV"|"$CALL_DEV") continue ;; esac
    # skip Pi ISP/codec nodes, keep USB/UVC cameras
    if udevadm info -q property -n "$d" 2>/dev/null | grep -q '^ID_BUS=usb'; then echo "$d"; return 0; fi
  done
  return 1
}

if [ "$CAMERA_DEVICE" = auto ]; then
  SRC="$(find_camera)" || { log "no USB camera found — plug it in; retrying"; exit 1; }
else
  SRC="$CAMERA_DEVICE"
fi
[ -e "$SRC" ] || { log "camera $SRC not present — retrying"; exit 1; }

# ─── run ffmpeg, falling back to simpler modes if the camera refuses ─
run() {   # run FORMAT SIZE FPS
  local fmt="$1" size="$2" fps="$3"
  local in=(-f v4l2 -thread_queue_size 8)
  [ -n "$fmt" ] && in+=(-input_format "$fmt")
  in+=(-video_size "$size" -framerate "$fps" -i "$SRC")
  log "streaming $SRC (${fmt:-native} ${size}@${fps}) → $VISION_DEV + $CALL_DEV"
  ffmpeg -hide_banner -loglevel warning -nostdin \
    -fflags nobuffer -flags low_delay \
    "${in[@]}" \
    -filter_complex "[0:v]format=yuyv422,split=2[vision][call]" \
    -map "[vision]" -f v4l2 "$VISION_DEV" \
    -map "[call]"   -f v4l2 "$CALL_DEV"
}

for mode in "$CAMERA_FORMAT $CAMERA_SIZE $CAMERA_FPS" " $CAMERA_SIZE $CAMERA_FPS" " 640x480 30"; do
  read -r fmt size fps <<<"$mode"
  [ -z "$fps" ] && { fps="$size"; size="$fmt"; fmt=""; }
  started=$(date +%s)
  run "$fmt" "$size" "$fps"
  # ran for a while → camera unplugged / crashed: let systemd restart us
  if [ $(( $(date +%s) - started )) -gt 10 ]; then
    log "ffmpeg stopped — restarting"
    exit 1
  fi
  log "camera refused ${fmt:-native} ${size}@${fps} — trying a simpler mode"
done
log "camera could not be opened in any mode"
exit 1
