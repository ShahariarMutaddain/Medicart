#!/usr/bin/env bash
# MediCart tests (no hardware needed). Run from the repo root:  bash tests/run_all.sh
# Needs: g++, python3 with pyserial requests numpy opencv-python-headless flask;
#        the browser test also needs Playwright for Python (+ Chromium).
set -euo pipefail
cd "$(dirname "$0")"
echo "── 1. Firmware 3.4 (the LFR code + Nov 11 reverse turning, identical outputs + Pi protocol + keys), host simulation"
g++ -std=c++17 -Wall -Wno-unused -I firmware -include array -x c++ firmware/sim.cpp -o firmware/sim && ./firmware/sim | grep -E "^(FAIL|ALL|FAILED)"
sed "s/const bool REVERSE_TURN_ASSIST = true;/const bool REVERSE_TURN_ASSIST = false;/" ../arduino/medicart/medicart.ino > firmware/fw_noassist.ino && g++ -std=c++17 -Wall -Wno-unused -I firmware -DNOASSIST -include array -x c++ firmware/sim.cpp -o firmware/sim_noassist && ./firmware/sim_noassist | grep -E "^(FAIL|ALL|FAILED)"; rm -f firmware/fw_noassist.ino firmware/sim_noassist
echo "── 2. Test sketch (arduino/medicart_test)"
g++ -std=c++17 -Wall -Wno-unused -I firmware -x c++ firmware/sim_test.cpp -o firmware/sim_test && ./firmware/sim_test | grep -E "^(FAIL|ALL|FAILED)"
echo "── 3. Real medicart.py + Rifat's dashboard + Firebase bridge + firmware 3.4 in real time"
g++ -std=c++17 -O1 -I firmware -DFIRMWARE='"../../arduino/medicart/medicart.ino"' -x c++ e2e/fwhost.cpp -o e2e/fwhost3
python3 e2e/run_v3.py 2>/dev/null | grep -E "^(PASS|FAIL)|passed"
if python3 -c "import playwright" 2>/dev/null; then
  echo "── 4. Vercel console Drive page + cart banner + Pi dashboard in Chromium"
  python3 browser/run_ui_v3.py 2>/dev/null | grep -E "^(PASS|FAIL)|passed"
echo "── 5. Doctor call: voice announcement, then auto-answer"
( cd browser && python3 run_call_voice.py 2>/dev/null | grep -E "^(FAIL|[0-9]+/[0-9]+ passed)" )
else
  echo "── 4. skipped (pip install playwright && playwright install chromium)"
fi
echo "── 6. ESP32 oximeter engine (host simulation)"
bash esp32/run.sh | tail -3
