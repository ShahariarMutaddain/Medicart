/*
 * ════════════════════════════════════════════════════════════════
 *  MediCart — ESP32 Vital Sensor Firmware  v2.1.0
 *  IUB Robotics Team · Independent University, Bangladesh
 *
 *  MAX30105 (HR + SpO2)  →  MQTT over TLS (HiveMQ public broker)
 *  Wi-Fi is set from a phone through a captive portal — no reflashing.
 * ════════════════════════════════════════════════════════════════
 *
 *  Wiring
 *    MAX30105 SDA → GPIO 25      MAX30105 SCL → GPIO 26
 *    MAX30105 INT → GPIO 33      I2C address 0x57
 *    BOOT button  → GPIO 0 (on-board)   Status LED → GPIO 2 (on-board)
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
 *  Libraries (Arduino Library Manager)
 *    • PubSubClient by Nick O'Leary
 *    • SparkFun MAX3010x Pulse and Proximity Sensor Library
 *  Board: "ESP32 Dev Module" (esp32 by Espressif, core 2.x or 3.x)
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <Wire.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"

// ─── Firmware identity ──────────────────────────────────────────
#define FW_VERSION        "2.1.0"

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
#define SDA_PIN           25
#define SCL_PIN           26
#define INT_PIN           33
#define BOOT_PIN          0
#define LED_PIN           2

// ─── Timing ─────────────────────────────────────────────────────
#define WIFI_BOOT_TIMEOUT_MS     20000UL
#define PORTAL_TIMEOUT_MS        300000UL   // 5 min, then reboot and retry saved Wi-Fi
#define WIFI_LOST_RESTART_MS     300000UL   // offline 5 min while running → reboot
#define MQTT_RETRY_MS            5000UL
#define PUBLISH_INTERVAL_MS      1000UL
#define STATUS_INTERVAL_MS       30000UL
#define SENSOR_RETRY_MS          10000UL
#define BOOT_HOLD_RUNNING_MS     5000UL

#define FINGER_THRESHOLD  50000
#define BUFFER_SIZE       100
#define BATCH             25

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
MAX30105 particleSensor;

const IPAddress AP_IP(192, 168, 4, 1);
String apName;
String scanJson = "[]";
unsigned long portalStartedAt = 0;
unsigned long restartAt = 0;

bool sensorOk = false;
unsigned long lastSensorRetry = 0;

uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];
byte newSamples = 0;
byte primeIndex = 0;
bool bufferPrimed = false;

int32_t spo2;
int8_t  validSPO2;
int32_t heartRate;
int8_t  validHeartRate;

int  hrSamples[4];
int  spo2Samples[4];
int  sampleCount = 0;
int  avgHeartRate = 0;
int  avgSpO2 = 0;
bool isMeasurementComplete = false;
long currentIR = 0, currentRed = 0;
bool fingerDetected = false;

unsigned long lastPublish = 0;
unsigned long lastStatus = 0;
unsigned long lastMqttAttempt = 0;
unsigned long wifiLostSince = 0;
unsigned long lastWifiKick = 0;
unsigned long bootPressedAt = 0;
unsigned long uptimeStart = 0;

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
//  Sensor
// ════════════════════════════════════════════════════════════════
bool initSensor() {
  Wire.begin(SDA_PIN, SCL_PIN);
  pinMode(INT_PIN, INPUT_PULLUP);
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println(F("[sensor] MAX30105 not found — check wiring (SDA 25, SCL 26)."));
    return false;
  }
  // ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange
  particleSensor.setup(60, 4, 2, 100, 411, 4096);
  Serial.println(F("[sensor] MAX30105 ready."));
  bufferPrimed = false;
  newSamples = 0;
  primeIndex = 0;
  return true;
}

void resetMeasurement() {
  sampleCount = 0;
  avgHeartRate = 0;
  avgSpO2 = 0;
  isMeasurementComplete = false;
}

// Called after every 25 fresh samples (≈1 s at 25 Hz effective rate).
void processBatch() {
  currentIR  = irBuffer[BUFFER_SIZE - 1];
  currentRed = redBuffer[BUFFER_SIZE - 1];
  fingerDetected = (currentIR >= FINGER_THRESHOLD);

  if (!fingerDetected) {
    resetMeasurement();
  } else if (!isMeasurementComplete) {
    maxim_heart_rate_and_oxygen_saturation(irBuffer, BUFFER_SIZE, redBuffer,
                                           &spo2, &validSPO2, &heartRate, &validHeartRate);
    if (validHeartRate && validSPO2 && heartRate > 40 && heartRate < 200 && spo2 > 70) {
      hrSamples[sampleCount]   = heartRate;
      spo2Samples[sampleCount] = spo2;
      sampleCount++;
      if (sampleCount >= 4) {
        int sumHR = 0, sumSp = 0;
        for (int i = 0; i < 4; i++) { sumHR += hrSamples[i]; sumSp += spo2Samples[i]; }
        avgHeartRate = sumHR / 4;
        avgSpO2      = sumSp / 4;
        isMeasurementComplete = true;
      }
    }
  }
}

// Non-blocking: drains the sensor FIFO each loop so Wi-Fi, MQTT and the
// web server never stall waiting for samples.
void serviceSensor() {
  if (!sensorOk) {
    if (millis() - lastSensorRetry > SENSOR_RETRY_MS) {
      lastSensorRetry = millis();
      sensorOk = initSensor();
      if (sensorOk && mqtt.connected()) mqtt.publish(TOPIC_STATUS, statusJson("online").c_str(), true);
    }
    return;
  }

  particleSensor.check();
  while (particleSensor.available()) {
    uint32_t red = particleSensor.getRed();
    uint32_t ir  = particleSensor.getIR();
    particleSensor.nextSample();

    if (!bufferPrimed) {
      // first fill: 100 samples (≈4 s) before the first estimate
      redBuffer[primeIndex] = red; irBuffer[primeIndex] = ir;
      if (++primeIndex >= BUFFER_SIZE) {
        primeIndex = 0;
        bufferPrimed = true;
        processBatch();
        memmove(redBuffer, redBuffer + BATCH, (BUFFER_SIZE - BATCH) * sizeof(uint32_t));
        memmove(irBuffer,  irBuffer  + BATCH, (BUFFER_SIZE - BATCH) * sizeof(uint32_t));
        newSamples = 0;
      }
      continue;
    }

    redBuffer[BUFFER_SIZE - BATCH + newSamples] = red;
    irBuffer[BUFFER_SIZE - BATCH + newSamples]  = ir;
    if (++newSamples >= BATCH) {
      processBatch();
      memmove(redBuffer, redBuffer + BATCH, (BUFFER_SIZE - BATCH) * sizeof(uint32_t));
      memmove(irBuffer,  irBuffer  + BATCH, (BUFFER_SIZE - BATCH) * sizeof(uint32_t));
      newSamples = 0;
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
  int hr = fingerDetected ? avgHeartRate : 0;
  int sp = fingerDetected ? avgSpO2 : 0;
  const char* state;
  if (!sensorOk)                    state = "sensor_error";
  else if (!bufferPrimed)           state = "waiting";
  else if (!fingerDetected)         state = "no_finger";
  else if (isMeasurementComplete)   state = "locked";
  else if (sampleCount > 0)         state = "measuring";
  else                              state = "waiting";

  char payload[320];
  snprintf(payload, sizeof(payload),
    "{\"heartRate\":%d,\"spo2\":%d,\"ir\":%ld,\"red\":%ld,"
    "\"fingerDetected\":%s,\"state\":\"%s\",\"samplesCollected\":%d,"
    "\"deviceId\":\"%s\",\"uptime\":%lu,\"rssi\":%d}",
    hr, sp, currentIR, currentRed,
    fingerDetected ? "true" : "false", state, sampleCount,
    cfg.deviceId.c_str(), (millis() - uptimeStart) / 1000, (int)WiFi.RSSI());
  mqtt.publish(TOPIC_SENSORS, payload);

  Serial.printf("HR:%d SpO2:%d IR:%ld RED:%ld finger:%s state:%s\n",
                hr, sp, currentIR, currentRed, fingerDetected ? "yes" : "no", state);
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
  sensorOk = initSensor();
  lastSensorRetry = millis();

  bool forcePortal = takePortalFlag();
  if (forcePortal) Serial.println(F("[boot] setup portal requested"));

  if (forcePortal || cfg.ssid.isEmpty() || !connectWiFi(WIFI_BOOT_TIMEOUT_MS)) startPortal();
  else startRun();
}

void loop() {
  ledPattern();
  server.handleClient();

  if (restartAt && (long)(millis() - restartAt) >= 0) {
    Serial.println(F("[sys] restarting"));
    delay(100);
    ESP.restart();
  }

  if (mode == MODE_PORTAL) {
    dns.processNextRequest();
    serviceSensor();                          // keep the sensor warm while in setup
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
  serviceSensor();

  if (mqtt.connected()) {
    if (millis() - lastPublish >= PUBLISH_INTERVAL_MS) { lastPublish = millis(); publishSensor(); }
    if (millis() - lastStatus >= STATUS_INTERVAL_MS) { lastStatus = millis(); mqtt.publish(TOPIC_STATUS, statusJson("online").c_str(), true); }
  }
}
