// Host simulation of firmware 3.4: line following must be identical to the LFR code that ran on the robot.
#include "Arduino.h"
#include <vector>
#include <cmath>
#define abs(x) ((x)>0?(x):-(x))          // Arduino AVR's abs (works on floats)
unsigned long g_ms = 0; int g_pwm[64] = {0}; int g_dig[80] = {0}; int g_ir[5] = {1,1,1,1,1}; int g_an[5] = {0};
SerialMock Serial;
namespace orig {
#include "../../arduino/medicart_lfr_original.ino.txt"
}
#ifdef NOASSIST
#include "fw_noassist.ino"
#else
#include "../../arduino/medicart/medicart.ino"
#endif
static int fails = 0;
static int L() { return g_pwm[2] ? g_pwm[2] : -g_pwm[3]; }
static int R() { return g_pwm[4] ? g_pwm[4] : -g_pwm[5]; }
static void check(bool c, const char* what) { printf("%s %s  (L=%d R=%d)\n", c ? "PASS" : "FAIL", what, L(), R()); if (!c) fails++; }
static bool has(const char* s) { return Serial.out.find(s) != std::string::npos; }
static void line(int p) { for (int n = 0; n < 5; n++) g_ir[n] = ((p >> (4 - n)) & 1) ? 0 : 1; }   // 1 = black = LOW
static void runNew(unsigned long ms) { for (unsigned long i = 0; i < ms; i++) { g_ms++; loop(); } }

int main() {
  // ---- 1. identical PWM to the original LFR code, every millisecond ----
  srand(11);
  std::vector<int> seq; std::vector<int> hold;
  for (int i = 0; i < 4000; i++) {
    int r = rand() % 10;
    int pat = r < 2 ? 0 : (r < 4 ? rand() % 32 : (r < 7 ? (1 << (rand() % 5)) : (3 << (rand() % 4))));
#ifndef NOASSIST
    if (pat == 0b01000 || pat == 0b10000 || pat == 0b00010 || pat == 0b00001) pat = 0b00100;
#endif
    seq.push_back(pat);
    hold.push_back(1 + rand() % 120);                 // pattern held 1..120 ms (also the lost-line search)
  }
  std::vector<std::array<int,4>> a, b;
  g_ms = 1000; memset(g_pwm, 0, sizeof g_pwm);
  orig::setup();
  unsigned long origRemainder = orig::motorSlewRemainder;
  for (size_t k = 0; k < seq.size(); k++) { line(seq[k]); for (int t = 0; t < hold[k]; t++) { g_ms++; orig::loop(); a.push_back({g_pwm[2], g_pwm[3], g_pwm[4], g_pwm[5]}); } }
  // new firmware: Pi says SAFE every 100 ms; the follow run starts at the same millisecond
  memset(g_pwm, 0, sizeof g_pwm); g_ms = 0; Serial.out.clear();
  setup(); Serial.feed("VIEW 0\n"); for (int i = 0; i < 999; i++) { g_ms++; loop(); }
  // The slew rate keeps a sub-step remainder; its phase only depends on how long each board has been on.
  // Start both from the same state as the original right after its setup(): remainder 0, last update 1 ms ago.
  motorSlewRemainder = origRemainder; lastMotorUpdateMs = g_ms;
  Serial.feed("SAFE\n");
  int sinceSafe = 0;
  for (size_t k = 0; k < seq.size(); k++) { line(seq[k]); for (int t = 0; t < hold[k]; t++) {
      g_ms++; if (++sinceSafe >= 100) { Serial.feed("SAFE\n"); sinceSafe = 0; } loop(); b.push_back({g_pwm[2], g_pwm[3], g_pwm[4], g_pwm[5]}); } }
  size_t diff = 0, first = 0; for (size_t i = 0; i < a.size(); i++) if (a[i] != b[i]) { if (!diff) first = i; diff++; }
  char m[160]; snprintf(m, sizeof m, "%zu ms of random line patterns: PWM on pins 2-5 identical to the LFR code (%zu differences%s)", a.size(), diff, diff ? ", first at ms " : "");
  if (diff) printf("first diff at %zu: orig %d %d %d %d  new %d %d %d %d\n", first, a[first][0], a[first][1], a[first][2], a[first][3], b[first][0], b[first][1], b[first][2], b[first][3]);
#ifdef NOASSIST
  check(diff == 0, "REVERSE_TURN_ASSIST = false: identical to today's LFR code, every millisecond");
  printf(fails ? "FAILED %d\n" : "ALL PASS\n", fails); return fails ? 1 : 0;
#else
  check(diff == 0, "assist on: all other patterns identical to today's LFR code, every millisecond");
#endif
  // ---- reverse turning (Nov 11 logic) ----
  {
    memset(g_pwm, 0, sizeof g_pwm); g_ms = 0; viewOn = false; manualMode = false; lfrTestUntilMs = 0;
    setup(); Serial.feed("VIEW 0\n");
    auto hold = [&](int p, int ms) { line(p); for (int i = 0; i < ms; i++) { g_ms++; if (i % 100 == 0) Serial.feed("SAFE\n"); loop(); } };
    hold(0b00100, 400); check(L() == 50 && R() == 50, "centre: 50 / 50");
    hold(0b01000, 20);  check(L() > 40 && L() < 50, "slide left starts smoothly (no jump)");
    hold(0b01000, 500); check(L() == -15 && R() == 50, "only S2: slide left = left BACK 15, right 50");
    hold(0b10000, 500); check(L() == -50 && R() == 50, "only S1: sharp left = left BACK 50, right 50");
    hold(0b00100, 600); check(L() == 50 && R() == 50, "back on centre: 50 / 50");
    hold(0b00010, 500); check(L() == 50 && R() == -15, "only S4: slide right = right BACK 15");
    hold(0b00001, 500); check(L() == 50 && R() == -50, "only S5: sharp right = right BACK 50");
    hold(0b11000, 500); check(L() >= 0 && R() == 50, "S1+S2 (not a single sensor): today's code, no reverse");
    Serial.feed("STOP\n"); for (int i = 0; i < 400; i++) { g_ms++; loop(); } check(L() == 0 && R() == 0, "Pi STOP while turning: smooth stop to 0");
  }

  // ---- 2. the LFR code's manual keys (p, w, a, d, s, x) ----
  memset(g_pwm, 0, sizeof g_pwm); g_ms = 0; viewOn = true; manualMode = false; Serial.out.clear();
  orig::manualMode = false; orig::stopMotors();
  setup(); runNew(10);
  Serial.feed("p\n"); runNew(5); Serial.feed("w\n"); runNew(300);
  check(L() == 50 && R() == 50, "p then w: forward 50 (smooth start, like the LFR code)");
  Serial.feed("a\n"); runNew(300); check(L() == 0 && R() == 50, "a: left 0 / right 50");
  Serial.feed("d\n"); runNew(400); check(L() == 50 && R() == 0, "d: left 50 / right 0");
  Serial.feed("s\n"); runNew(600); check(L() == -50 && R() == -50, "s: back 50");
  Serial.feed("x\n"); runNew(60); check(L() < 0 && L() > -50, "x: slows down smoothly (not a hard stop)"); runNew(200); check(L() == 0 && R() == 0, "x: stopped after the ramp");
  Serial.feed("w"); runNew(400); check(L() == 50, "w with no line ending also works");
  Serial.feed(" "); runNew(250); check(L() == 0 && R() == 0, "space: smooth stop");
  Serial.feed("p\n"); runNew(5); check(!manualMode, "p again: manual off");
  // ---- 3. architecture ----
  line(0b00100); runNew(800);
  check(L() == 0 && R() == 0, "no Pi: line following does NOT start by itself");
  Serial.feed("SAFE\n"); runNew(300); check(L() == 50 && R() == 50, "Pi SAFE: line following (centre = 50/50)");
  Serial.feed("STOP\n"); runNew(50); check(L() > 0 && L() < 50, "Pi STOP (person): ramps down smoothly");
  runNew(150); check(L() == 0 && R() == 0, "Pi STOP: stopped within 0.2 s (from 50)");
  Serial.feed("SAFE\n"); runNew(300); check(L() == 50, "SAFE again: continues");
  runNew(1000); check(L() == 0 && R() == 0, "no Pi command for 750 ms: stops by itself (smoothly)");
  for (int i = 0; i < 10; i++) { Serial.feed("MODE:MANUAL\nDRIVE:40,-40\n"); runNew(100); } check(L() == 40 && R() == -40, "Pi manual: DRIVE:40,-40");
  Serial.feed("MODE:MANUAL\nDRIVE:250,250\n"); for (int i = 0; i < 10; i++) { Serial.feed("DRIVE:250,250\n"); runNew(100); } check(L() == 180 && R() == 180, "DRIVE capped at 180");
  runNew(750); check(L() > 0 && L() < 180, "Pi manual, no command for 750 ms: ramping down");
  runNew(700); check(L() == 0 && R() == 0, "...stopped (0.6 s ramp from 180)");
  Serial.feed("QR:3\n"); runNew(2); check(g_dig[8] == HIGH, "QR:3 -> pin 8");
  Serial.out.clear(); Serial.feed("STAT\n"); runNew(2); check(has("STAT mode=MANUAL") && has("line=00100") && has("fw=3.4"), "STAT for the Pi");
    // ---- 4. bench line following without the Pi ----
  Serial.feed("l\n"); runNew(300); check(L() == 50 && R() == 50, "l: line following without the Pi");
  runNew(30000); check(L() == 50, "l: still following after 30 s (no Pi needed)");
  runNew(31000); check(L() == 0 && R() == 0, "l: stops after 60 s");
  Serial.feed("l\n"); runNew(300); Serial.feed("x\n"); runNew(250); check(L() == 0 && R() == 0, "x ends the l run (smooth stop)");
  Serial.feed("l\n"); runNew(300); Serial.feed("STOP\n"); runNew(250); check(L() == 0 && !lfrTestUntilMs, "a Pi command ends the l run");
  printf(fails ? "FAILED %d\n" : "ALL PASS\n", fails); return fails ? 1 : 0;
}
