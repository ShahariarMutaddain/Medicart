# ESP32 vital sensor

Reads a UART pulse-oximeter module and publishes heart rate and SpO₂ over MQTT (TLS) to the doctor console and the cart screen. It works on its own: no wire to the Pi or the Arduino, only 5 V power and Wi-Fi.

| Path | What it is |
|---|---|
| [`medicart_sensor/medicart_sensor.ino`](medicart_sensor/medicart_sensor.ino) | **Firmware 3.0.0** source |
| [`prebuilt/medicart_sensor_v3.0.0_merged.bin`](prebuilt/) | ready to flash at `0x0` (esp32 core 2.0.17); checksum in `SHA256SUMS` |
| [`legacy/`](legacy/) | the earlier MAX30105 (I²C) version 2.1 and its image, for the old sensor board |

## What 3.0.0 does

- **Motion detection** from the pulse wave. While the finger moves, beats and SpO₂ are ignored and the last good values are **held** (SpO₂ up to 10 s) instead of dropping to `--`.
- **Heart rate:** 7-beat median, rejects beats that don't fit the rhythm.
- **SpO₂:** learns which byte of the module's packet is SpO₂ and saves it in flash (`spo2idx`), then a 5-reading median.
- A finger counts as removed only after **0.4 s** out of range, so a short glitch doesn't wipe the reading.
- **Locked** after 4 steady seconds (the web shows *Measuring 1…4 of 4*).
- Runs the sensor in its own **FreeRTOS task**, so Wi-Fi and MQTT never make it miss data.
- Finds the module by itself if the wires are swapped or the baud rate differs.

## Wiring

| ESP32 | Module |
|---|---|
| GPIO 16 (RX2) | TX |
| GPIO 17 (TX2) | RX |
| 3V3 or 5 V | VCC |
| GND | GND |

Board **ESP32 Dev Module** (WROOM). BOOT = GPIO 0, LED = GPIO 2.

## Flash

- **Browser:** [esptool-js](https://espressif.github.io/esptool-js/) → Connect → `0x0` → `prebuilt/medicart_sensor_v3.0.0_merged.bin` → Program → press EN.
- **Arduino IDE:** *esp32 by Espressif* + library *PubSubClient* → board **ESP32 Dev Module** → Upload.

## Wi-Fi setup

Join `MediCart-Sensor-XXXX` (password `medicart`) → the page opens (or `http://192.168.4.1`) → admin password `medicart-admin` → your network → **set a new admin password** under *Advanced* → Save. The setup portal also opens when the saved Wi-Fi can't be joined within 20 s, or when you hold BOOT for 5 s.

| LED | Meaning |
|---|---|
| fast blink | setup portal |
| slow blink | connecting |
| solid | online |

## Serial Monitor (115200)

`INFO` status · `RAW` show the module's packets · `SPO2IDX n` force the SpO₂ byte (`0` = learn again).

## MQTT

`broker.hivemq.com:8883`, topics `<prefix>/01/sensors` and `<prefix>/01/status`. `TOPIC_PREFIX` must match `MQTT_TOPIC_PREFIX` in [`web-src/js/config.js`](../web-src/js/config.js). Payload: [`docs/PROTOCOLS.md`](../docs/PROTOCOLS.md#3-mqtt-esp32--web).

## Test without hardware

```bash
bash tests/esp32/run.sh     # simulated module: pulse wave, movement, finger on/off
```
