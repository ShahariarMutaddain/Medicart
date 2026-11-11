#!/usr/bin/env bash
# MediCart 3.1 tests (no hardware needed). Run from the repo root:  bash tests/run_all.sh
# Needs: g++, python3 with pyserial requests numpy opencv-python-headless flask;
#        the browser test also needs Playwright for Python (+ Chromium).
set -euo pipefail
cd "$(dirname "$0")"
echo "── 1. Firmware 3.1 (original LFR logic, identical outputs + Pi protocol + tools), host simulation"
g++ -std=c++17 -Wall -Wno-unused -I firmware -x c++ firmware/sim.cpp -o firmware/sim && ./firmware/sim | grep -E "^(FAIL|ALL|FAILED)"
echo "── 2. Test sketch (arduino/medicart_test)"
g++ -std=c++17 -Wall -Wno-unused -I firmware -x c++ firmware/sim_test.cpp -o firmware/sim_test && ./firmware/sim_test | grep -E "^(FAIL|ALL|FAILED)"
echo "── 3. Real medicart.py + Rifat's dashboard + Firebase bridge + firmware 3.1 in real time"
g++ -std=c++17 -O1 -I firmware -DFIRMWARE='"../../arduino/medicart/medicart.ino"' -x c++ e2e/fwhost.cpp -o e2e/fwhost3
python3 e2e/run_v3.py 2>/dev/null | grep -E "^(PASS|FAIL)|passed"
if python3 -c "import playwright" 2>/dev/null; then
  echo "── 4. Vercel console Drive page + cart banner + Pi dashboard in Chromium"
  python3 browser/run_ui_v3.py 2>/dev/null | grep -E "^(PASS|FAIL)|passed"
else
  echo "── 4. skipped (pip install playwright && playwright install chromium)"
fi
