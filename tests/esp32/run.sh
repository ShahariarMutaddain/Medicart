#!/usr/bin/env bash
# Host test of the ESP32 oximeter engine (simulated module: pulse wave, movement, finger on/off).
set -e
cd "$(dirname "$0")"
{ echo '#include <cstdint>'; echo '#include <cmath>'; echo '#include <cstdlib>'; echo 'using std::abs;'
  awk '/==== OXIMETER ENGINE BEGIN ====/{f=1;next} /==== OXIMETER ENGINE END ====/{f=0} f' ../../esp32_sensor/medicart_sensor/medicart_sensor.ino; } > engine_extract.h
g++ -std=c++17 -O1 test_engine.cpp -o test_engine && ./test_engine
