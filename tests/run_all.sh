#!/usr/bin/env bash
# MediCart control system tests (no hardware needed). Run from the repo root:  bash tests/run_all.sh
# Needs: g++, python3 with pyserial + requests; the browser test also needs Playwright for Python.
set -euo pipefail
cd "$(dirname "$0")"
echo "── 1. Firmware 2.1 logic (host simulation)"
g++ -std=c++17 -Wall -I firmware -x c++ firmware/sim.cpp -o firmware/sim && ./firmware/sim
echo "── 2. Control service end to end (firmware in real time + fake medicart.py + fake Firebase)"
g++ -std=c++17 -O1 -I firmware -DFIRMWARE='"../../arduino/medicart/medicart.ino"' -x c++ e2e/fwhost.cpp -o e2e/fwhost21
g++ -std=c++17 -O1 -I firmware -DFIRMWARE='"../../arduino/medicart.cpp.original"' -x c++ e2e/fwhost.cpp -o e2e/fwhostorig
python3 e2e/run_e2e.py
if python3 -c "import playwright" 2>/dev/null; then
  echo "── 3. Console Drive page + cart banner in Chromium"
  python3 browser/run_drive_ui.py
else
  echo "── 3. skipped (pip install playwright && playwright install chromium)"
fi
