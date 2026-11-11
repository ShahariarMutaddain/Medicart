#!/usr/bin/env bash
# MediCart — launches the YOLO safety / line-following vision script headless.
set -eu
. /etc/medicart/medicart.env
: "${VISION_SCRIPT:?set VISION_SCRIPT in /etc/medicart/medicart.env}"
: "${VISION_PYTHON:=python3}"
export MEDICART_VIDEO_SOURCE="${MEDICART_VIDEO_SOURCE:-10}"   # /dev/video10 = MediCart-Vision
# no DISPLAY → medicart.py skips cv2.imshow and runs headless
unset DISPLAY WAYLAND_DISPLAY
cd "$(dirname "$VISION_SCRIPT")"
exec "$VISION_PYTHON" -u "$VISION_SCRIPT"
