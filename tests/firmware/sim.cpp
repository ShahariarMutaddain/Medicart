// Host simulation of arduino/medicart/medicart.ino (firmware 3.1): proves the line following is identical to the original LFR code, plus the Pi protocol and the Serial Monitor tools.
#include "Arduino.h"
#include <cstring>
#include <vector>
unsigned long g_ms = 0; int g_pwm[64] = {0}; int g_dig[80] = {0}; int g_ir[5] = {1,1,1,1,1}; int g_an[5] = {0};
SerialMock Serial;
#include "EEPROM.h"
EEPROMMock EEPROM;
inline void delay(unsigned long ms) { g_ms += ms; }
namespace orig {
#include "../../arduino/medicart_lfr_original.ino.txt"
}
#include "../../arduino/medicart/medicart.ino"
static int fails = 0;
static void run(unsigned long ms) { for (unsigned long i = 0; i < ms; i++) { g_ms++; loop(); } }
static bool has(const char* s) { return Serial.out.find(s) != std::string::npos; }
static void check(bool c, const char* what) { printf("%s %s  (pins 2,3,4,5 = %d %d %d %d)\n", c ? "PASS" : "FAIL", what, g_pwm[2], g_pwm[3], g_pwm[4], g_pwm[5]); if (!c) fails++; }
static void raw(const char* line) { for (int n = 0; n < 5; n++) g_ir[n] = line[n] == '1' ? 0 : 1; }   // "1" = black under that sensor = raw LOW
static bool pins(int a, int b, int c, int d) { return g_pwm[2] == a && g_pwm[3] == b && g_pwm[4] == c && g_pwm[5] == d; }
int main() {
  // ---- 1. identical to the original LFR code: same PWM on pins 2-5 for every cycle ----
  srand(7);
  std::vector<int> seq;
  for (int i = 0; i < 3000; i++) { int r = rand() % 10; seq.push_back(r < 3 ? 0 : (r < 5 ? (rand() % 32) : (1 << (rand() % 5)))); }
  std::vector<std::vector<int>> a, b;
  g_ms = 0; orig::setup();
  for (int p : seq) { for (int n = 0; n < 5; n++) g_ir[n] = (p >> (4 - n)) & 1 ? 0 : 1; orig::loop(); a.push_back({g_pwm[2], g_pwm[3], g_pwm[4], g_pwm[5]}); }
  memset(g_pwm, 0, sizeof g_pwm); g_ms = 100000; Serial.out.clear();
  setup(); Serial.feed("VIEW 0\nSAFE\n"); run(1);
  for (int p : seq) { for (int n = 0; n < 5; n++) g_ir[n] = (p >> (4 - n)) & 1 ? 0 : 1; Serial.feed("SAFE\n"); run(50); b.push_back({g_pwm[2], g_pwm[3], g_pwm[4], g_pwm[5]}); }
  int diff = 0; for (size_t i = 0; i < a.size(); i++) if (a[i] != b[i]) diff++;
  char m[120]; snprintf(m, sizeof m, "3000 random cycles: PWM on pins 2-5 identical to the original LFR code (%d differences)", diff);
  check(diff == 0, m);

  // ---- 2. the original table, one by one (Pi says SAFE) ----
  memset(g_pwm, 0, sizeof g_pwm); g_ms = 200000; Serial.out.clear(); viewOn = true; setup(); run(10);
  check(pins(0,0,0,0) && has("FW medicart 3.1"), "boots stopped, FW medicart 3.1");
  struct { const char* line; int a, b, c, d; const char* text; } t[] = {
    {"00100",38,0,38,0,"forwrd"}, {"01000",0,15,38,0,"slide left (active reverse brake)"}, {"10000",0,38,38,0,"left (sharp)"},
    {"00010",38,0,0,15,"slide right (active reverse brake)"}, {"00001",38,0,0,38,"right (sharp)"},
    {"11111",0,0,0,0,"stop (line junction)"}, {"01100",38,0,38,0,"forwrd (default)"} };
  for (auto &c : t) { raw(c.line); Serial.out.clear(); Serial.feed("SAFE\n"); run(60);
    char msg[100]; snprintf(msg, sizeof msg, "line %s -> %s", c.line, c.text); check(pins(c.a,c.b,c.c,c.d) && has(c.text), msg); }
  check(has("S1: 0\tS2: 1\tS3: 1\tS4: 0\tS5: 0\t| PWM_L: 38\tPWM_R: 38"), "Serial Monitor shows the original S1..S5 | PWM line");
  raw("00100"); Serial.feed("SAFE\n"); run(60);
  raw("00000"); Serial.out.clear(); Serial.feed("SAFE\n"); run(50); check(pins(38,0,38,0) && has("forwrd (glitch ignored)"), "line gone 1 cycle -> keeps forward (glitch filter)");
  Serial.feed("SAFE\n"); run(50); check(pins(0,0,0,0) && has("stop (confirmed off-line)"), "line gone 2 cycles -> stop");
  // ---- 3. architecture ----
  raw("00100"); Serial.feed("SAFE\n"); run(60); check(pins(38,0,38,0), "Pi SAFE -> line following");
  Serial.feed("STOP\n"); run(2); check(pins(0,0,0,0), "Pi STOP (person) -> stops within 2 ms");
  Serial.feed("SAFE\n"); run(5); check(pins(38,0,38,0), "SAFE again -> continues at once");
  run(800); check(pins(0,0,0,0), "no Pi command for 750 ms -> stops by itself");
  for (int i = 0; i < 5; i++) { Serial.feed("MODE:MANUAL\nDRIVE:40,-40\n"); run(100); } check(pins(40,0,0,40), "MODE:MANUAL + DRIVE:40,-40");
  Serial.feed("MODE:MANUAL\nDRIVE:250,250\n"); run(5); check(pins(180,0,180,0), "DRIVE capped at 180");
  run(800); check(pins(0,0,0,0), "manual stops 750 ms after the last command");
  Serial.feed("QR:3\n"); run(2); check(g_dig[8] == HIGH, "QR:3 -> pin 8");
  Serial.out.clear(); Serial.feed("STAT\n"); run(2); check(has("STAT mode=MANUAL") && has("line=00100") && has("fw=3.1"), "STAT for the Pi");
  // ---- 4. Serial Monitor ----
  Serial.feed("v\n"); run(2);
  Serial.feed("w\n"); run(100); check(pins(38,0,38,0), "w: forward at 38"); run(1000); check(pins(0,0,0,0), "w: stops after 1 s");
  Serial.feed("a\n"); run(50); check(pins(0,38,38,0), "a: pivot left like turnLeft()");
  Serial.feed("d\n"); run(50); check(pins(38,0,0,38), "d: pivot right like turnRight()");
  Serial.feed("s\n"); run(50); check(pins(0,38,0,38), "s: back"); Serial.feed("x\n"); run(2); check(pins(0,0,0,0), "x: stop");
  raw("00100"); Serial.feed("l\n"); run(100); check(pins(38,0,38,0), "l: line following without the Pi");
  run(30000); check(pins(38,0,38,0), "l: still following after 30 s");
  run(31000); check(pins(0,0,0,0), "l: stops after 60 s");
  Serial.feed("l\n"); run(100); Serial.feed("x\n"); run(2); check(pins(0,0,0,0), "x ends the l run");
  Serial.feed("FLIPL\n"); run(2); Serial.feed("w\n"); run(50); check(pins(0,38,38,0), "FLIPL reverses only the left wheel");
  Serial.feed("x\nFACTORY\n"); run(2);
  // finger test (array facing up: nothing = raw 0)
  for (int n = 0; n < 5; n++) g_ir[n] = 0;
  Serial.out.clear(); Serial.feed("t\n"); run(60); check(has("No fingers reads raw 00000") && pins(0,0,0,0), "t: finger test starts, no finger = stop");
  g_ir[1] = 1; run(60); check(pins(0,15,38,0), "finger on S2 -> slide left");
  g_ir[1] = 0; g_ir[4] = 1; run(60); check(pins(38,0,0,38), "finger on S5 -> right (sharp)");
  Serial.feed("x\n"); run(2); check(pins(0,0,0,0), "x ends the finger test");
  printf(fails ? "FAILED %d\n" : "ALL PASS\n", fails); return fails ? 1 : 0;
}
