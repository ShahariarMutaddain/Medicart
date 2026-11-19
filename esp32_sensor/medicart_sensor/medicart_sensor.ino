/*
 * ════════════════════════════════════════════════════════════════
 *  MediCart — ESP32 Vital Sensor Firmware  v3.0.0
 *  IUB Robotics Team · Independent University, Bangladesh
 *
 *  UART pulse-oximeter module (HR + SpO2)  →  MQTT over TLS (HiveMQ public broker)
 *  → MediCart web console (Vitals) and cart screen.
 *  Wi-Fi is set from a phone through a captive portal — no reflashing.
 * ════════════════════════════════════════════════════════════════
 *
 *  Wiring (ESP32 Hardware Serial 2)
 *    Oximeter TX  → GPIO 16 (RX2)        Oximeter RX → GPIO 17 (TX2)
 *    (Swapped wires or another speed? It finds the module by itself and tells you in the Serial Monitor.)
 *    Oximeter VCC → 3V3 (or 5V if the module needs it)   GND → GND
 *    BOOT button  → GPIO 0 (on-board)    Status LED  → GPIO 2 (on-board)
 *
 *  What 3.0 does better than the bench sketch
 *    • A finger is "removed" only after 0.4 s out of range: a short glitch while
 *      moving no longer wipes the reading.
 *    • Movement is detected from the pulse wave. While moving, beats and SpO2
 *      readings are ignored and the last good values are HELD (SpO2 up to 10 s),
 *      instead of SpO2 dropping to --.
 *    • Heart rate: 7-beat median + rejection of beats that don't fit the rhythm.
 *    • SpO2: learns which byte of the module's packet is SpO2 (saved in flash),
 *      then reads only that byte (70–100 %), 5-reading median.
 *    • The sensor runs in its own task, so Wi-Fi / MQTT never make it miss data.
 *    • "Locked" after 4 steady seconds (the web shows Measuring 1..4 of 4).
 *
 *  Serial Monitor (115200): live line every 0.5 s, plus commands
 *    INFO        status and learned SpO2 byte
 *    RAW         show / hide the module's SpO2 packets (hex), for checking
 *    SPO2IDX n   force the SpO2 byte position (1-11);  SPO2IDX 0 = learn again
 *
 *  Setup portal opens when
 *    • no Wi-Fi has been saved yet, or
 *    • the saved Wi-Fi cannot be joined within 20 s at power-up, or
 *    • BOOT is held for 5 s while running (device restarts into setup).
 *      (Don't hold BOOT while plugging in — that starts the ESP32's
 *       flashing bootloader instead of this firmware.)
 *  Join Wi-Fi "MediCart-Sensor-XXXX" (password: medicart) — the page
 *  opens by itself, or browse to http://192.168.4.1
 *  While running, settings are also at http://medicart-sensor.local
 *
 *  LED:  fast blink = setup portal · slow blink = connecting · solid = online
 *
 *  MQTT (must match web-src/js/config.js)
 *    broker.hivemq.com:8883 (TLS) · topics iub-medicart-591a7e01d2/01/sensors and /01/status
 *    sensors JSON: heartRate, spo2, ir (= PPG wave), red (= pulse strength), fingerDetected,
 *                  state (waiting | no_finger | measuring | locked | sensor_error),
 *                  samplesCollected (0-4), motion, spo2Held, deviceId, uptime, rssi
 *
 *  Libraries (Arduino Library Manager):  PubSubClient by Nick O'Leary
 *  Board: "ESP32 Dev Module" (esp32 by Espressif, core 2.x or 3.x)
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <math.h>

// ─── Firmware identity ──────────────────────────────────────────
#define FW_VERSION        "3.0.0"

// ─── Defaults (overridable from the portal, stored in flash) ────
// HiveMQ public broker: free, no account, no login. Shared with everyone,
// so MediCart uses a long random topic prefix (must match js/config.js).
#define DEFAULT_MQTT_HOST "broker.hivemq.com"
#define DEFAULT_MQTT_PORT 8883
#define DEFAULT_MQTT_USER ""
#define DEFAULT_MQTT_PASS ""
// Bump when the defaults above change: devices then drop broker settings
// saved by older firmware and use the new defaults.
#define MQTT_PROFILE      2
#define DEFAULT_DEVICE_ID "MEDICART-01"
#define DEFAULT_ADMIN_PASS "medicart-admin"   // change it in the portal on first setup

#define SETUP_AP_PASS     "medicart"          // WPA2 key of the setup hotspot (8+ chars)
#define MDNS_NAME         "medicart-sensor"

#define TOPIC_PREFIX      "iub-medicart-591a7e01d2"
#define TOPIC_SENSORS     TOPIC_PREFIX "/01/sensors"
#define TOPIC_STATUS      TOPIC_PREFIX "/01/status"

// ─── Pins ───────────────────────────────────────────────────────
#define OXI_RX_PIN        16    // ← oximeter TX
#define OXI_TX_PIN        17    // → oximeter RX
#define OXI_BAUD          115200
#define BOOT_PIN          0
#define LED_PIN           2

// ─── Timing ─────────────────────────────────────────────────────
#define WIFI_BOOT_TIMEOUT_MS     20000UL
#define PORTAL_TIMEOUT_MS        300000UL   // 5 min, then reboot and retry saved Wi-Fi
#define WIFI_LOST_RESTART_MS     300000UL   // offline 5 min while running → reboot
#define MQTT_RETRY_MS            5000UL
#define PUBLISH_INTERVAL_MS      1000UL
#define STATUS_INTERVAL_MS       30000UL
#define BOOT_HOLD_RUNNING_MS     5000UL
#define SERIAL_PRINT_MS          500UL

// ════════════════════════════════════════════════════════════════
//  Oximeter engine (plain C++, runs in its own task)
//  Module packet: 0x41 0x42 + 12 bytes.  body[0] = type
//    type 0x01: PPG sample, 16-bit little-endian in body[2], body[3]
//    type 0x02: module result; one byte of body[1..11] is SpO2 %
// ════════════════════════════════════════════════════════════════
// ==== OXIMETER ENGINE BEGIN ====
namespace oxi {

// ---- tuning (change here) ----
const uint16_t RAW_MIN          = 50;      // PPG outside 50..4000 = no finger
const uint16_t RAW_MAX          = 4000;
const uint32_t FINGER_ON_MS     = 300;     // in range this long  → finger placed
const uint32_t FINGER_OFF_MS    = 400;     // out of range this long → finger removed (shorter = movement glitch)
const uint32_t LINK_LOST_MS     = 1000;    // no PPG samples this long → finger removed
const uint32_t LINK_TIMEOUT_MS  = 2000;    // no packets at all → module not connected
const uint32_t MOTION_HOLD_MS   = 1500;    // after the last sign of movement, wait this long
const float    MOTION_FACTOR    = 3.0f;    // swing > 3 × normal pulse height = movement
const float    MOTION_MIN       = 15.0f;   // ...and at least this many raw units
const uint32_t MOTION_RELEARN_MS = 5000;   // "moving" for 5 s straight → relearn the pulse height
const uint32_t REFRACTORY_MS    = 350;     // no second beat within 350 ms (max 171 BPM)
const uint32_t IBI_MIN_MS       = 350;
const uint32_t IBI_MAX_MS       = 1500;    // min 40 BPM
const float    IBI_TOLERANCE    = 0.30f;   // a beat 30 % off the rhythm is an artefact
const uint8_t  IBI_REJECT_RESET = 4;       // 4 off-rhythm beats in a row = the rhythm really changed
const uint32_t PULSE_LOST_MS    = 4000;    // still finger, no beat for 4 s → HR --
const uint32_t SPO2_HOLD_MS     = 10000;   // keep the last good SpO2 this long (movement)
const uint32_t SPO2_FRESH_MS    = 3000;    // older than this = "held" (shown, but not counted as steady)
const uint8_t  SPO2_LOW         = 70;      // valid range once the SpO2 byte is known
const uint8_t  SPO2_HIGH        = 100;
const uint8_t  SCAN_LOW         = 88;      // range used to find the SpO2 byte (bench sketch rule)
const uint8_t  SCAN_HIGH        = 99;
const uint16_t LEARN_VOTES      = 12;      // packets needed before the SpO2 byte is fixed
const uint8_t  LOCK_SECONDS     = 4;       // steady seconds before "locked"
const int      MEDIAN_LEN       = 7;
const int      SPO2_MED_LEN     = 5;

struct Output {
  int  hr = 0, spo2 = 0, ppg = 0, amp = 0;
  bool finger = false, motion = false, link = false, spo2Held = false;
  uint8_t progress = 0;      // steady seconds 0..LOCK_SECONDS
  int8_t  spo2Index = 0;     // 0 = still learning
  uint32_t packets = 0;
};

class Engine {
public:
  static int medianInt(const int* v, int n) {
    int s[16];
    for (int i = 0; i < n; i++) s[i] = v[i];
    for (int i = 1; i < n; i++) { int k = s[i], j = i - 1; while (j >= 0 && s[j] > k) { s[j + 1] = s[j]; j--; } s[j + 1] = k; }
    return n ? s[n / 2] : 0;
  }

  int8_t spo2Index = 0;          // 1..11 once known (loaded from flash), 0 = learn
  bool   learnedNow = false;     // true once after learning: the caller saves spo2Index
  bool   rawDump = false;
  void (*dump)(const uint8_t* body) = nullptr;

  void setIndex(int idx) {
    spo2Index = (idx >= 1 && idx <= 11) ? (int8_t)idx : 0;
    for (int i = 0; i < 12; i++) { votes[i] = 0; hrLike[i] = 0; noFingerHits[i] = 0; }
    fingerPkts = noFingerPkts = 0;
    spo2Count = 0; spo2Pos = 0; spo2Filt = 0;
  }

  void feed(uint8_t b, uint32_t now) {
    switch (sync) {
      case 0: if (b == 0x41) sync = 1; break;
      case 1: if (b == 0x42) { sync = 2; bodyIdx = 0; } else if (b != 0x41) sync = 0; break;
      default:
        body[bodyIdx++] = b;
        if (bodyIdx >= 12) {
          sync = 0;
          packets++;
          lastPacketMs = now;
          if (body[0] == 0x01) sample((uint16_t)(body[2] | (body[3] << 8)), now);
          else if (body[0] == 0x02) result(now);
        }
    }
  }

  // call often: finger timeout, pulse lost, steady-seconds counter
  void tick(uint32_t now) {
    if (finger && now - lastSampleMs > LINK_LOST_MS) fingerRemoved();
    if (finger && !moving(now) && bpm && now - lastBeatSeenMs > PULSE_LOST_MS) { bpm = 0; ibiCount = 0; }
    if (now - lastSecondMs >= 1000) {
      lastSecondMs = now;
      int sp = spo2Out(now);
      if (!finger) progress = 0;
      else if (moving(now)) progress = progress > 2 ? progress - 2 : 0;
      else if (bpm > 0 && sp > 0 && now - spo2LastGoodMs <= SPO2_FRESH_MS) { if (progress < LOCK_SECONDS) progress++; }
      else if (bpm == 0 || sp == 0) progress = 0;
    }
  }

  Output out(uint32_t now) const {
    Output o;
    o.link = packets > 0 && now - lastPacketMs < LINK_TIMEOUT_MS;
    o.finger = finger && o.link;
    o.motion = o.finger && moving(now);
    o.hr = o.finger ? bpm : 0;
    o.spo2 = o.finger ? spo2Out(now) : 0;
    o.spo2Held = o.spo2 > 0 && now - spo2LastGoodMs > SPO2_FRESH_MS;
    o.ppg = ppg;
    o.amp = (int)(amp + 0.5f);
    o.progress = o.finger ? progress : 0;
    o.spo2Index = spo2Index;
    o.packets = packets;
    return o;
  }

private:
  // parser
  uint8_t sync = 0, bodyIdx = 0, body[12];
  uint32_t packets = 0, lastPacketMs = 0, lastSampleMs = 0, lastSecondMs = 0;
  // finger
  bool finger = false;
  uint32_t inSinceMs = 0, outSinceMs = 0;
  // wave
  int ppg = 0;
  float smooth4[4] = {0}, dc = 0, env = 5.0f, amp = 0, prevSmooth = 0, cycleMax = 0;
  int smoothIdx = 0, smoothFill = 0, ampBeats = 0;
  bool armed = true, wasMoving = false;
  uint32_t motionUntilMs = 0, motionSinceMs = 0;
  // heart rate
  int ibi[MEDIAN_LEN] = {0}, ibiIdx = 0, ibiCount = 0, rejects = 0, bpm = 0;
  uint32_t lastBeatMs = 0, lastBeatSeenMs = 0;
  // SpO2
  int spo2Buf[SPO2_MED_LEN] = {0}, spo2Count = 0, spo2Pos = 0, spo2Filt = 0;
  uint32_t spo2LastGoodMs = 0;
  uint16_t votes[12] = {0}, hrLike[12] = {0}, noFingerHits[12] = {0}, fingerPkts = 0, noFingerPkts = 0;
  uint8_t progress = 0;

  bool moving(uint32_t now) const { return (int32_t)(motionUntilMs - now) > 0; }

  int spo2Out(uint32_t now) const {
    return (spo2Filt > 0 && now - spo2LastGoodMs <= SPO2_HOLD_MS) ? spo2Filt : 0;
  }

  int medianIbi() const { return medianInt(ibi, ibiCount); }

  void resetWave() {
    for (int i = 0; i < 4; i++) smooth4[i] = 0;
    smoothIdx = smoothFill = 0; dc = 0; env = 5.0f; armed = true; prevSmooth = 0; cycleMax = 0;
  }

  void fingerRemoved() {
    finger = false;
    resetWave();
    amp = 0; ampBeats = 0; motionUntilMs = 0; wasMoving = false;
    ibiCount = 0; ibiIdx = 0; rejects = 0; bpm = 0; lastBeatMs = 0;
    spo2Count = 0; spo2Pos = 0; spo2Filt = 0; spo2LastGoodMs = 0;
    progress = 0;
  }

  void markMotion(uint32_t now) {
    if (!moving(now)) motionSinceMs = now;
    motionUntilMs = now + MOTION_HOLD_MS;
  }

  void sample(uint16_t raw, uint32_t now) {
    lastSampleMs = now;
    if (raw < RAW_MIN || raw > RAW_MAX) {
      inSinceMs = 0;
      if (!outSinceMs) outSinceMs = now ? now : 1;
      if (finger) {
        if (now - outSinceMs >= FINGER_OFF_MS) fingerRemoved();
        else markMotion(now);                      // short drop-out while on the finger = movement
      }
      return;
    }
    outSinceMs = 0;
    if (!finger) {
      if (!inSinceMs) inSinceMs = now ? now : 1;
      if (now - inSinceMs < FINGER_ON_MS) return;
      fingerRemoved();                             // clean start
      finger = true;
      lastBeatSeenMs = now;
    }
    ppg = raw;

    smooth4[smoothIdx] = (float)raw;
    smoothIdx = (smoothIdx + 1) % 4;
    if (smoothFill < 4) { smoothFill++; if (smoothFill < 4) return; }
    float s = (smooth4[0] + smooth4[1] + smooth4[2] + smooth4[3]) * 0.25f;

    if (dc == 0.0f) { dc = s; prevSmooth = s; return; }
    dc = dc * 0.985f + s * 0.015f;
    float ac = s - dc;
    float step = fabsf(s - prevSmooth);
    prevSmooth = s;

    // movement: a swing or a jump far bigger than a normal pulse
    if (ampBeats >= 3 && (fabsf(ac) > fmaxf(MOTION_FACTOR * amp, MOTION_MIN) || step > fmaxf(MOTION_FACTOR * amp, MOTION_MIN))) markMotion(now);
    bool mv = moving(now);
    if (mv && now - motionSinceMs > MOTION_RELEARN_MS) {
      // "moving" for too long: the pulse itself probably got stronger → relearn its height
      ampBeats = 0; amp = 0; motionUntilMs = 0; mv = false;
    }
    if (mv) { wasMoving = true; armed = false; return; }
    if (wasMoving) {                               // movement just ended: re-centre, restart beat timing
      wasMoving = false; dc = s; env = amp > 2.5f ? amp : 5.0f; armed = false; lastBeatMs = 0; cycleMax = 0;
      return;
    }

    if (ac > env) env = ac;
    else { env *= 0.993f; if (env < 2.5f) env = 2.5f; }
    if (ac > cycleMax) cycleMax = ac;              // pulse height of this beat

    if (ac > env * 0.50f && armed && (lastBeatMs == 0 || now - lastBeatMs >= REFRACTORY_MS)) {
      beat(now);
      armed = false;
    }
    if (ac < 0.0f) armed = true;
  }

  void beat(uint32_t now) {
    lastBeatSeenMs = now;
    if (cycleMax > 0.0f) {                         // learn the normal pulse height from the previous beat
      amp = ampBeats == 0 ? cycleMax : amp * 0.8f + cycleMax * 0.2f;
      if (ampBeats < 100) ampBeats++;
    }
    cycleMax = 0.0f;
    if (lastBeatMs) {
      uint32_t d = now - lastBeatMs;
      if (d >= IBI_MIN_MS && d <= IBI_MAX_MS) {
        bool fits = true;
        if (ibiCount >= 3) {
          int m = medianIbi();
          fits = fabsf((float)d - (float)m) <= IBI_TOLERANCE * (float)m;
        }
        if (fits || ++rejects >= IBI_REJECT_RESET) {
          if (!fits) { ibiCount = 0; ibiIdx = 0; }   // the rhythm really changed: start over
          rejects = 0;
          ibi[ibiIdx] = (int)d;
          ibiIdx = (ibiIdx + 1) % MEDIAN_LEN;
          if (ibiCount < MEDIAN_LEN) ibiCount++;
          if (ibiCount >= 3) { int m = medianIbi(); bpm = m > 0 ? (int)(60000L / m) : 0; }
        }
      }
    }
    lastBeatMs = now;
  }

  bool disqualified(int i) const {               // byte sits in 88..99 even with no finger → not SpO2
    return noFingerPkts >= 5 && noFingerHits[i] * 2 >= noFingerPkts;
  }

  void result(uint32_t now) {
    if (rawDump && dump) dump(body);
    if (!finger) {
      if (noFingerPkts < 60000) noFingerPkts++;
      for (int i = 1; i < 12; i++) if (body[i] >= SCAN_LOW && body[i] <= SCAN_HIGH && noFingerHits[i] < 60000) noFingerHits[i]++;
      return;
    }
    if (fingerPkts < 60000) fingerPkts++;

    if (spo2Index == 0) {                          // learn which byte is SpO2
      int best = 0, bestScore = 0;
      for (int i = 1; i < 12; i++) {
        if (body[i] >= SCAN_LOW && body[i] <= SCAN_HIGH) {
          votes[i]++;
          if (bpm > 0 && abs((int)body[i] - bpm) <= 3) hrLike[i]++;   // that byte follows the pulse → it's HR
        }
        int score = (int)votes[i] - (int)hrLike[i];
        if (!disqualified(i) && score > bestScore) { bestScore = score; best = i; }
      }
      if (best && bestScore >= LEARN_VOTES && votes[best] * 10 >= fingerPkts * 6) {
        spo2Index = (int8_t)best;
        learnedNow = true;
      }
    }

    int v = -1;
    if (spo2Index > 0) {
      if (body[spo2Index] >= SPO2_LOW && body[spo2Index] <= SPO2_HIGH) v = body[spo2Index];
    } else {
      for (int i = 1; i <= 9; i++) {
        if (body[i] >= SCAN_LOW && body[i] <= SCAN_HIGH && !disqualified(i) &&
            !(bpm > 0 && abs((int)body[i] - bpm) <= 3 && hrLike[i] * 2 > votes[i])) { v = body[i]; break; }
      }
    }
    if (v < 0 || moving(now)) return;             // no reading, or taken while moving: keep the last good one

    spo2Buf[spo2Pos] = v;
    spo2Pos = (spo2Pos + 1) % SPO2_MED_LEN;
    if (spo2Count < SPO2_MED_LEN) spo2Count++;
    spo2Filt = medianInt(spo2Buf, spo2Count);
    spo2LastGoodMs = now;
  }
};

}  // namespace oxi
// ==== OXIMETER ENGINE END ====

// ════════════════════════════════════════════════════════════════
//  Configuration (NVS)
// ════════════════════════════════════════════════════════════════
struct Config {
  String ssid, pass;
  String mqttHost, mqttUser, mqttPass;
  uint16_t mqttPort;
  String deviceId, adminPass;
};
Config cfg;
Preferences prefs;

void loadConfig() {
  prefs.begin("medicart", true);
  cfg.ssid      = prefs.getString("ssid", "");
  cfg.pass      = prefs.getString("pass", "");
  cfg.mqttHost  = prefs.getString("mhost", DEFAULT_MQTT_HOST);
  cfg.mqttPort  = prefs.getUShort("mport", DEFAULT_MQTT_PORT);
  cfg.mqttUser  = prefs.getString("muser", DEFAULT_MQTT_USER);
  cfg.mqttPass  = prefs.getString("mpass", DEFAULT_MQTT_PASS);
  cfg.deviceId  = prefs.getString("devid", DEFAULT_DEVICE_ID);
  cfg.adminPass = prefs.getString("admin", DEFAULT_ADMIN_PASS);
  uint8_t profile = prefs.getUChar("mprof", 0);
  prefs.end();
  if (profile != MQTT_PROFILE) {            // firmware moved to a new broker
    cfg.mqttHost = DEFAULT_MQTT_HOST;
    cfg.mqttPort = DEFAULT_MQTT_PORT;
    cfg.mqttUser = DEFAULT_MQTT_USER;
    cfg.mqttPass = DEFAULT_MQTT_PASS;
    prefs.begin("medicart", false);
    prefs.putString("mhost", cfg.mqttHost);
    prefs.putUShort("mport", cfg.mqttPort);
    prefs.putString("muser", cfg.mqttUser);
    prefs.putString("mpass", cfg.mqttPass);
    prefs.putUChar("mprof", MQTT_PROFILE);
    prefs.end();
  }
}

void saveConfig() {
  prefs.begin("medicart", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("mhost", cfg.mqttHost);
  prefs.putUShort("mport", cfg.mqttPort);
  prefs.putString("muser", cfg.mqttUser);
  prefs.putString("mpass", cfg.mqttPass);
  prefs.putString("devid", cfg.deviceId);
  prefs.putString("admin", cfg.adminPass);
  prefs.end();
}

void setPortalFlag(bool on) {
  prefs.begin("medicart", false);
  prefs.putBool("portal", on);
  prefs.end();
}

bool takePortalFlag() {
  prefs.begin("medicart", false);
  bool on = prefs.getBool("portal", false);
  if (on) prefs.putBool("portal", false);
  prefs.end();
  return on;
}

// ════════════════════════════════════════════════════════════════
//  Globals
// ════════════════════════════════════════════════════════════════
enum Mode { MODE_PORTAL, MODE_RUN };
Mode mode = MODE_RUN;

WebServer server(80);
DNSServer dns;
WiFiClientSecure tlsClient;
PubSubClient mqtt(tlsClient);

const IPAddress AP_IP(192, 168, 4, 1);
String apName;
String scanJson = "[]";
unsigned long portalStartedAt = 0;
unsigned long restartAt = 0;

bool sensorOk = false;                      // oximeter packets arriving

unsigned long lastPublish = 0;
unsigned long lastStatus = 0;
unsigned long lastMqttAttempt = 0;
unsigned long wifiLostSince = 0;
unsigned long lastWifiKick = 0;
unsigned long bootPressedAt = 0;
unsigned long uptimeStart = 0;
unsigned long lastPrint = 0;

// ════════════════════════════════════════════════════════════════
//  Helpers
// ════════════════════════════════════════════════════════════════
String jsonEscape(const String& s) {
  String o;
  o.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) { char b[7]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
    else o += c;
  }
  return o;
}

void ledPattern() {
  unsigned long t = millis();
  if (mode == MODE_PORTAL)              digitalWrite(LED_PIN, (t / 120) % 2);
  else if (mqtt.connected())            digitalWrite(LED_PIN, HIGH);
  else                                  digitalWrite(LED_PIN, (t / 600) % 2);
}

String statusJson(const char* status) {
  String s = "{\"device\":\"" + jsonEscape(cfg.deviceId) + "\",\"status\":\"" + status + "\"";
  if (strcmp(status, "online") == 0) {
    s += ",\"fw\":\"" FW_VERSION "\"";
    s += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
    s += ",\"rssi\":" + String(WiFi.RSSI());
    s += ",\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\"";
    s += ",\"sensor\":" + String(sensorOk ? "true" : "false");
  }
  s += "}";
  return s;
}

// ════════════════════════════════════════════════════════════════
//  Oximeter task (core 1, above loop()): reads Serial2 all the time,
//  so a slow Wi-Fi / TLS connect never makes it lose samples.
// ════════════════════════════════════════════════════════════════
oxi::Engine engine;
oxi::Output oxOut;                          // latest snapshot, guarded by oxMux
portMUX_TYPE oxMux = portMUX_INITIALIZER_UNLOCKED;
volatile int  cmdSpo2Index = -1;           // set by a Serial command, applied by the task
volatile bool cmdRawToggle = false;
volatile int  learnedIndexToSave = 0;      // set by the task, saved to flash by loop()

void dumpPacket(const uint8_t* body) {
  char line[96];
  int n = snprintf(line, sizeof(line), "RAW 02:");
  for (int i = 1; i < 12 && n < (int)sizeof(line) - 5; i++) n += snprintf(line + n, sizeof(line) - n, " %d=%u", i, body[i]);
  Serial.println(line);
}

// If no packets arrive, try the other wiring / speeds by itself (TX and RX swapped is the usual mistake).
struct OxiPort { int8_t rx, tx; uint32_t baud; };
const OxiPort OXI_PORTS[] = {
  {OXI_RX_PIN, OXI_TX_PIN, OXI_BAUD}, {OXI_TX_PIN, OXI_RX_PIN, OXI_BAUD},
  {OXI_RX_PIN, OXI_TX_PIN, 9600},     {OXI_TX_PIN, OXI_RX_PIN, 9600},
  {OXI_RX_PIN, OXI_TX_PIN, 57600},    {OXI_RX_PIN, OXI_TX_PIN, 38400},
};
const int OXI_PORT_COUNT = sizeof(OXI_PORTS) / sizeof(OXI_PORTS[0]);
const uint32_t OXI_TRY_MS = 3000;           // no packet for 3 s → try the next wiring / speed
volatile int oxiPort = 0;
volatile uint32_t oxiBytes = 0;             // bytes received on the current wiring / speed

void openOxiPort(int i) {
  Serial2.end();
  Serial2.setRxBufferSize(2048);
  Serial2.begin(OXI_PORTS[i].baud, SERIAL_8N1, OXI_PORTS[i].rx, OXI_PORTS[i].tx);
  oxiPort = i;
  oxiBytes = 0;
}

void oximeterTask(void*) {
  uint32_t lastSnap = 0, lastPackets = 0, lastPacketAt = millis();
  bool announced = false;
  for (;;) {
    int n = 0;
    while (Serial2.available() && n < 512) { engine.feed((uint8_t)Serial2.read(), millis()); n++; }
    oxiBytes += n;
    uint32_t now = millis();
    uint32_t packets = engine.out(now).packets;
    if (packets != lastPackets) {
      lastPackets = packets;
      lastPacketAt = now;
      if (!announced) {
        announced = true;
        Serial.printf("[sensor] oximeter found: its TX → GPIO%d, %lu baud%s\n", OXI_PORTS[oxiPort].rx,
                      (unsigned long)OXI_PORTS[oxiPort].baud, oxiPort ? "  (works; the wiring/speed differs from the default)" : "");
      }
    } else if (now - lastPacketAt > OXI_TRY_MS) {
      int next = (oxiPort + 1) % OXI_PORT_COUNT;
      if (oxiBytes > 0)
        Serial.printf("[sensor] %lu bytes on GPIO%d at %lu baud, but no oximeter packets\n", (unsigned long)oxiBytes,
                      OXI_PORTS[oxiPort].rx, (unsigned long)OXI_PORTS[oxiPort].baud);
      Serial.printf("[sensor] no packets: trying oximeter TX → GPIO%d at %lu baud\n", OXI_PORTS[next].rx, (unsigned long)OXI_PORTS[next].baud);
      openOxiPort(next);
      lastPacketAt = now;
      announced = false;
    }
    if (cmdSpo2Index >= 0) { engine.setIndex(cmdSpo2Index); cmdSpo2Index = -1; }
    if (cmdRawToggle) { engine.rawDump = !engine.rawDump; cmdRawToggle = false; }
    engine.tick(now);
    if (engine.learnedNow) { engine.learnedNow = false; learnedIndexToSave = engine.spo2Index; }
    if (now - lastSnap >= 50) {
      lastSnap = now;
      oxi::Output o = engine.out(now);
      portENTER_CRITICAL(&oxMux);
      oxOut = o;
      portEXIT_CRITICAL(&oxMux);
    }
    if (n == 0) vTaskDelay(1);
  }
}

oxi::Output vitals() {
  portENTER_CRITICAL(&oxMux);
  oxi::Output o = oxOut;
  portEXIT_CRITICAL(&oxMux);
  return o;
}

const char* stateOf(const oxi::Output& o) {
  if (!o.link) return o.packets ? "sensor_error" : "waiting";
  if (!o.finger) return "no_finger";
  if (o.progress >= oxi::LOCK_SECONDS) return "locked";
  return "measuring";
}

void startOximeter() {
  prefs.begin("medicart", true);
  int idx = prefs.getChar("spo2idx", 0);
  prefs.end();
  engine.setIndex(idx);
  engine.dump = dumpPacket;
  openOxiPort(0);
  xTaskCreatePinnedToCore(oximeterTask, "oximeter", 4096, nullptr, 3, nullptr, 1);
  Serial.printf("[sensor] oximeter on RX2=GPIO%d TX2=GPIO%d, SpO2 byte: %s\n", OXI_RX_PIN, OXI_TX_PIN,
                idx ? String(idx).c_str() : "learning");
}

// called from loop(): save a newly learned SpO2 byte, report link changes
void serviceOximeter() {
  if (learnedIndexToSave) {
    int idx = learnedIndexToSave;
    learnedIndexToSave = 0;
    prefs.begin("medicart", false);
    prefs.putChar("spo2idx", (int8_t)idx);
    prefs.end();
    Serial.printf("[sensor] SpO2 is byte %d of the module's result packet (saved)\n", idx);
  }
  bool link = vitals().link;
  if (link != sensorOk) {
    sensorOk = link;
    Serial.println(link ? F("[sensor] oximeter connected") : F("[sensor] no data from the oximeter: check its TX → GPIO 16, power and GND"));
    if (mode == MODE_RUN && mqtt.connected()) mqtt.publish(TOPIC_STATUS, statusJson("online").c_str(), true);
  }
}

// live line in the Serial Monitor, same style as the bench sketch
void printVitals() {
  oxi::Output o = vitals();
  char sp[8], hr[12];
  if (o.spo2 > 0) snprintf(sp, sizeof(sp), "%d%%", o.spo2); else strcpy(sp, "--%");
  if (o.hr > 0) snprintf(hr, sizeof(hr), "%d BPM", o.hr); else strcpy(hr, "-- BPM");
  Serial.printf("PPG_Wave:%d\tSpO2:%s\tPulse:%s", o.ppg, sp, hr);
  if (!o.link)                Serial.printf("  [No data from oximeter: listening on GPIO%d at %lu baud, %lu bytes]",
                                            OXI_PORTS[oxiPort].rx, (unsigned long)OXI_PORTS[oxiPort].baud, (unsigned long)oxiBytes);
  else if (!o.finger)         Serial.print(F("  [Insert Finger]"));
  else if (o.motion)          Serial.print(F("  [Hold still - movement, showing last reading]"));
  else if (o.hr == 0 || o.spo2 == 0) Serial.print(F("  [Stabilizing...]"));
  else if (o.progress >= oxi::LOCK_SECONDS) Serial.print(F("  [Locked]"));
  else                        Serial.printf("  [Measuring %d/%d]", o.progress, oxi::LOCK_SECONDS);
  if (o.spo2Held) Serial.print(F(" (SpO2 held)"));
  Serial.println();
}

// Serial Monitor commands: INFO, RAW, SPO2IDX n
String serialLine;
void serviceSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') { if (serialLine.length() < 40) serialLine += c; continue; }
    String cmd = serialLine; serialLine = ""; cmd.trim(); cmd.toUpperCase();
    if (cmd.isEmpty()) continue;
    if (cmd == "RAW") { cmdRawToggle = true; Serial.println(F("[sensor] RAW packets toggled")); }
    else if (cmd.startsWith("SPO2IDX")) {
      int idx = cmd.substring(7).toInt();
      if (idx < 0 || idx > 11) idx = 0;
      cmdSpo2Index = idx;
      prefs.begin("medicart", false); prefs.putChar("spo2idx", (int8_t)idx); prefs.end();
      Serial.printf("[sensor] SpO2 byte %s\n", idx ? String(idx).c_str() : "will be learned again");
    } else if (cmd == "INFO") {
      oxi::Output o = vitals();
      Serial.printf("[info] fw %s  oximeter %s (TX → GPIO%d, %lu baud, %lu bytes)  packets %lu  SpO2 byte %d  state %s  wifi %s  mqtt %s\n",
                    FW_VERSION, o.link ? "OK" : "no data", OXI_PORTS[oxiPort].rx, (unsigned long)OXI_PORTS[oxiPort].baud,
                    (unsigned long)oxiBytes, (unsigned long)o.packets, o.spo2Index, stateOf(o),
                    WiFi.status() == WL_CONNECTED ? "OK" : "--", mqtt.connected() ? "OK" : "--");
    } else {
      Serial.println(F("Commands: INFO  RAW  SPO2IDX n (0 = learn again)"));
    }
  }
}

// ════════════════════════════════════════════════════════════════
//  MQTT
// ════════════════════════════════════════════════════════════════
void mqttConnectOnce() {
  lastMqttAttempt = millis();
  Serial.print(F("[mqtt] connecting… "));
  String will = statusJson("offline");
  // unique client ID: on a shared broker a duplicate ID would kick us off
  String clientId = cfg.deviceId + "-" + WiFi.macAddress().substring(12);
  clientId.replace(":", "");
  bool anon = cfg.mqttUser.isEmpty();
  if (mqtt.connect(clientId.c_str(),
                   anon ? nullptr : cfg.mqttUser.c_str(), anon ? nullptr : cfg.mqttPass.c_str(),
                   TOPIC_STATUS, 1, true, will.c_str())) {
    Serial.println(F("connected"));
    mqtt.publish(TOPIC_STATUS, statusJson("online").c_str(), true);
    lastStatus = millis();
  } else {
    Serial.printf("failed rc=%d\n", mqtt.state());
  }
}

void publishSensor() {
  oxi::Output o = vitals();
  const char* state = stateOf(o);
  char payload[360];
  snprintf(payload, sizeof(payload),
    "{\"heartRate\":%d,\"spo2\":%d,\"ir\":%d,\"red\":%d,"
    "\"fingerDetected\":%s,\"state\":\"%s\",\"samplesCollected\":%d,"
    "\"motion\":%s,\"spo2Held\":%s,"
    "\"deviceId\":\"%s\",\"uptime\":%lu,\"rssi\":%d}",
    o.hr, o.spo2, o.ppg, o.amp,
    o.finger ? "true" : "false", state, o.progress,
    o.motion ? "true" : "false", o.spo2Held ? "true" : "false",
    cfg.deviceId.c_str(), (millis() - uptimeStart) / 1000, (int)WiFi.RSSI());
  mqtt.publish(TOPIC_SENSORS, payload);
}

// ════════════════════════════════════════════════════════════════
//  Web portal (setup hotspot + LAN settings page)
// ════════════════════════════════════════════════════════════════
const char PAGE_HTML[] PROGMEM = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>MediCart Sensor Setup</title>
<style>
:root{--n:#0A2647;--g:#C8861B;--p:#5B2C8E;--bg:#F2F5F9;--t:#0E1A2B;--m:#75859A;--l:#D6DEE9}
*{box-sizing:border-box}body{margin:0;font:15px/1.5 system-ui,-apple-system,Segoe UI,sans-serif;background:var(--bg);color:var(--t)}
header{background:var(--n);color:#fff;padding:22px 18px 26px}header small{color:#E6A537;font-weight:700;letter-spacing:.14em;font-size:11px}
header h1{margin:4px 0 0;font:600 24px Georgia,serif}main{max-width:480px;margin:-14px auto 24px;padding:0 14px}
.c{background:#fff;border-radius:14px;padding:18px;box-shadow:0 6px 20px -8px rgba(10,38,71,.25);margin-bottom:14px}
label{display:block;font-size:12px;font-weight:600;color:#45556A;margin:12px 0 5px}
input,select{width:100%;height:44px;border:1px solid var(--l);border-radius:10px;padding:0 12px;font:inherit;background:#fff}
input:focus,select:focus{outline:none;border-color:#1B5292;box-shadow:0 0 0 3px rgba(27,82,146,.18)}
button{width:100%;height:48px;border:0;border-radius:12px;background:var(--n);color:#fff;font:inherit;font-weight:700;margin-top:18px;cursor:pointer}
button.s{background:none;color:#1B5292;height:auto;width:auto;margin:0;font-size:13px;padding:0}
.row{display:flex;justify-content:space-between;align-items:center}.m{color:var(--m);font-size:12px}
details{margin-top:14px}summary{cursor:pointer;font-weight:600;color:var(--p)}
dl{display:grid;grid-template-columns:auto 1fr;gap:4px 14px;margin:0;font-size:13px}dt{color:var(--m)}dd{margin:0;font-weight:600;text-align:right;overflow-wrap:anywhere}
.badge{display:inline-block;padding:2px 8px;border-radius:99px;font-size:11px;font-weight:700;background:#E5F4EA;color:#15803D}.badge.x{background:#FCE5E6;color:#C41E2A}
</style></head><body>
<header><small>IUB ROBOTICS TEAM</small><h1>MediCart Sensor Setup</h1></header>
<main>
<div class="c"><dl id="info"><dt>Loading…</dt><dd></dd></dl></div>
<form class="c" method="POST" action="/save" autocomplete="off">
<label>Admin password</label><input name="admin" type="password" required>
<div class="row" style="margin-top:12px"><label style="margin:0">Wi-Fi network</label><button type="button" class="s" onclick="scan(1)">Rescan</button></div>
<select id="nets" onchange="if(this.value)document.getElementById('ssid').value=this.value"><option value="">Scanning…</option></select>
<label>Network name (SSID)</label><input id="ssid" name="ssid" required maxlength="32">
<label>Wi-Fi password</label><input name="pass" type="password" maxlength="64" placeholder="Leave empty for open networks">
<details><summary>Advanced</summary>
<label>MQTT host</label><input name="mhost" id="mhost" maxlength="100">
<label>MQTT TLS port</label><input name="mport" id="mport" type="number" min="1" max="65535">
<label>MQTT username</label><input name="muser" id="muser" maxlength="64" placeholder="None (public broker)">
<label>MQTT password</label><input name="mpass" type="password" maxlength="64" placeholder="Unchanged">
<label style="display:flex;gap:8px;align-items:center;font-weight:500"><input type="checkbox" name="manon" value="1" style="width:auto;height:auto"> Broker needs no login (public broker)</label>
<label>Device ID</label><input name="devid" id="devid" maxlength="32">
<label>New admin password</label><input name="newadmin" type="password" minlength="6" maxlength="64" placeholder="Unchanged (6+ characters)">
</details>
<button>Save &amp; connect</button>
<p class="m">The sensor restarts and joins the network. Reconnect your phone to your normal Wi-Fi afterwards.</p>
</form></main>
<script>
function scan(r){var s=document.getElementById('nets');s.innerHTML='<option value="">Scanning…</option>';
fetch('/scan.json'+(r?'?rescan=1':'')).then(function(x){return x.json()}).then(function(l){s.innerHTML='';
var o=document.createElement('option');o.value='';o.textContent=l.length?'Choose a network…':'No networks found';s.appendChild(o);
l.forEach(function(n){var e=document.createElement('option');e.value=n.ssid;e.textContent=n.ssid+'  ('+n.rssi+' dBm'+(n.open?', open':'')+')';s.appendChild(e)})})}
fetch('/info.json').then(function(x){return x.json()}).then(function(i){var d=document.getElementById('info');d.innerHTML='';
[['Device',i.device],['Firmware',i.fw],['Saved Wi-Fi',i.ssid||'—'],['Sensor',i.sensor?'OK':'Not detected'],['MAC',i.mac]].forEach(function(r){
var t=document.createElement('dt');t.textContent=r[0];var v=document.createElement('dd');v.textContent=r[1];d.appendChild(t);d.appendChild(v)});
['mhost','mport','muser','devid'].forEach(function(k){document.getElementById(k).value=i[k]});
if(i.ssid)document.getElementById('ssid').value=i.ssid});scan(0);
</script></body></html>)HTML";

String messagePage(const String& title, const String& body, bool ok) {
  String p = F("<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
               "<title>MediCart Sensor</title><style>body{margin:0;font:15px/1.5 system-ui,sans-serif;background:#F2F5F9;color:#0E1A2B}"
               "header{background:#0A2647;color:#fff;padding:22px 18px}h1{margin:0;font:600 22px Georgia,serif}"
               "main{max-width:480px;margin:18px auto;padding:0 14px}.c{background:#fff;border-radius:14px;padding:18px;border-left:5px solid ");
  p += ok ? "#15803D" : "#C41E2A";
  p += F("}a{color:#1B5292;font-weight:600}</style></head><body><header><h1>MediCart Sensor</h1></header><main><div class=\"c\"><h2>");
  p += title;
  p += F("</h2><p>");
  p += body;
  p += F("</p></div></main></body></html>");
  return p;
}

void doScan() {
  int n = WiFi.scanNetworks(false, true);
  String j = "[";
  int added = 0;
  for (int i = 0; i < n && added < 25; i++) {
    String s = WiFi.SSID(i);
    if (s.isEmpty()) continue;
    bool dup = false;
    for (int k = 0; k < i; k++) if (WiFi.SSID(k) == s) { dup = true; break; }
    if (dup) continue;
    if (added++) j += ",";
    j += "{\"ssid\":\"" + jsonEscape(s) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
         ",\"open\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  j += "]";
  WiFi.scanDelete();
  scanJson = j;
}

void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", PAGE_HTML);
}

void handleScan() {
  if (server.hasArg("rescan") || scanJson == "[]") doScan();
  server.send(200, "application/json", scanJson);
}

void handleInfo() {
  String j = "{\"device\":\"" + jsonEscape(cfg.deviceId) + "\",\"fw\":\"" FW_VERSION "\"";
  j += ",\"ssid\":\"" + jsonEscape(cfg.ssid) + "\"";
  j += ",\"sensor\":" + String(sensorOk ? "true" : "false");
  j += ",\"mac\":\"" + WiFi.macAddress() + "\"";
  j += ",\"mhost\":\"" + jsonEscape(cfg.mqttHost) + "\",\"mport\":" + String(cfg.mqttPort);
  j += ",\"muser\":\"" + jsonEscape(cfg.mqttUser) + "\",\"devid\":\"" + jsonEscape(cfg.deviceId) + "\"}";
  server.send(200, "application/json", j);
}

void handleSave() {
  if (server.arg("admin") != cfg.adminPass) {
    server.send(403, "text/html", messagePage("Wrong admin password", "Nothing was changed. <a href=\"/\">Go back</a>.", false));
    return;
  }
  String ssid = server.arg("ssid");
  ssid.trim();
  if (ssid.isEmpty() || ssid.length() > 32) {
    server.send(400, "text/html", messagePage("Network name missing", "Enter the Wi-Fi name. <a href=\"/\">Go back</a>.", false));
    return;
  }
  String pass = server.arg("pass");
  if (!pass.isEmpty() && pass.length() < 8) {
    server.send(400, "text/html", messagePage("Password too short", "WPA2 passwords have at least 8 characters. <a href=\"/\">Go back</a>.", false));
    return;
  }
  cfg.ssid = ssid;
  cfg.pass = pass;

  String v;
  v = server.arg("mhost"); v.trim(); if (!v.isEmpty()) cfg.mqttHost = v;
  v = server.arg("mport"); v.trim(); if (v.toInt() > 0 && v.toInt() < 65536) cfg.mqttPort = (uint16_t)v.toInt();
  if (server.arg("manon") == "1") {
    cfg.mqttUser = ""; cfg.mqttPass = "";
  } else {
    v = server.arg("muser"); v.trim(); if (!v.isEmpty()) cfg.mqttUser = v;
    v = server.arg("mpass");           if (!v.isEmpty()) cfg.mqttPass = v;
  }
  v = server.arg("devid"); v.trim(); if (!v.isEmpty()) cfg.deviceId = v;
  v = server.arg("newadmin");        if (v.length() >= 6) cfg.adminPass = v;
  saveConfig();

  server.send(200, "text/html", messagePage("Saved",
    "The sensor will restart and join <b>" + ssid + "</b>. If it cannot connect, the "
    "<b>" + apName + "</b> hotspot comes back within about 30 seconds.", true));
  restartAt = millis() + 1500;
}

void handleCaptive() {
  // Any unknown URL (OS connectivity checks included) lands on the setup page.
  server.sendHeader("Location", String("http://") + AP_IP.toString() + "/", true);
  server.send(302, "text/plain", "");
}

void registerRoutes(bool captive) {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/scan.json", HTTP_GET, handleScan);
  server.on("/info.json", HTTP_GET, handleInfo);
  server.on("/save", HTTP_POST, handleSave);
  if (captive) server.onNotFound(handleCaptive);
  else server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
}

void startPortal() {
  mode = MODE_PORTAL;
  Serial.println(F("[portal] starting setup hotspot"));
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  delay(100);
  doScan();                                   // scan before the AP takes the radio

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char suffix[6];
  snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
  apName = String("MediCart-Sensor-") + suffix;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(apName.c_str(), SETUP_AP_PASS);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", AP_IP);
  registerRoutes(true);
  server.begin();
  portalStartedAt = millis();
  Serial.printf("[portal] join \"%s\" (password %s) → http://%s\n", apName.c_str(), SETUP_AP_PASS, AP_IP.toString().c_str());
}

// ════════════════════════════════════════════════════════════════
//  Wi-Fi (station)
// ════════════════════════════════════════════════════════════════
bool connectWiFi(unsigned long timeoutMs) {
  Serial.printf("[wifi] joining \"%s\"", cfg.ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(MDNS_NAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    digitalWrite(LED_PIN, (millis() / 600) % 2);
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] connected, IP %s, RSSI %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  Serial.println(F("[wifi] could not connect"));
  return false;
}

void startRun() {
  mode = MODE_RUN;
  tlsClient.setInsecure();                    // HiveMQ Cloud TLS without pinned CA
  mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(10);

  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);
  registerRoutes(false);
  server.begin();
  Serial.printf("[web] settings at http://%s.local or http://%s\n", MDNS_NAME, WiFi.localIP().toString().c_str());

  uptimeStart = millis();
  mqttConnectOnce();
}

// ════════════════════════════════════════════════════════════════
//  Arduino entry points
// ════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(150);
  Serial.println(F("\n═══ MediCart Sensor " FW_VERSION " ═══"));
  pinMode(LED_PIN, OUTPUT);
  pinMode(BOOT_PIN, INPUT_PULLUP);

  loadConfig();
  startOximeter();                            // runs from now on, in its own task

  bool forcePortal = takePortalFlag();
  if (forcePortal) Serial.println(F("[boot] setup portal requested"));

  if (forcePortal || cfg.ssid.isEmpty() || !connectWiFi(WIFI_BOOT_TIMEOUT_MS)) startPortal();
  else startRun();
}

void loop() {
  ledPattern();
  server.handleClient();
  serviceSerialCommands();
  serviceOximeter();
  if (millis() - lastPrint >= SERIAL_PRINT_MS) { lastPrint = millis(); printVitals(); }

  if (restartAt && (long)(millis() - restartAt) >= 0) {
    Serial.println(F("[sys] restarting"));
    delay(100);
    ESP.restart();
  }

  if (mode == MODE_PORTAL) {
    dns.processNextRequest();
    if (!cfg.ssid.isEmpty() && millis() - portalStartedAt > PORTAL_TIMEOUT_MS && WiFi.softAPgetStationNum() == 0) {
      Serial.println(F("[portal] timeout — retrying saved Wi-Fi"));
      ESP.restart();
    }
    return;
  }

  // ─── RUN mode ───
  // BOOT held 5 s → reboot into the setup portal
  if (digitalRead(BOOT_PIN) == LOW) {
    if (!bootPressedAt) bootPressedAt = millis();
    else if (millis() - bootPressedAt > BOOT_HOLD_RUNNING_MS) {
      Serial.println(F("[boot] BOOT held — rebooting into setup portal"));
      setPortalFlag(true);
      delay(100);
      ESP.restart();
    }
  } else {
    bootPressedAt = 0;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiLostSince) { wifiLostSince = millis(); Serial.println(F("[wifi] lost connection")); }
    if (millis() - lastWifiKick > 10000) { lastWifiKick = millis(); WiFi.reconnect(); }
    if (millis() - wifiLostSince > WIFI_LOST_RESTART_MS) ESP.restart();
  } else {
    if (wifiLostSince) { Serial.println(F("[wifi] back online")); wifiLostSince = 0; }
    if (!mqtt.connected() && millis() - lastMqttAttempt > MQTT_RETRY_MS) mqttConnectOnce();
  }

  mqtt.loop();

  if (mqtt.connected()) {
    if (millis() - lastPublish >= PUBLISH_INTERVAL_MS) { lastPublish = millis(); publishSensor(); }
    if (millis() - lastStatus >= STATUS_INTERVAL_MS) { lastStatus = millis(); mqtt.publish(TOPIC_STATUS, statusJson("online").c_str(), true); }
  }
}
