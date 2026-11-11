// Host simulation of arduino/medicart_test/medicart_test.ino
#include "Arduino.h"
unsigned long g_ms = 0; int g_pwm[64] = {0}; int g_dig[80] = {0}; int g_ir[5] = {0}; int g_an[5] = {100,100,900,100,100};
SerialMock Serial;
#include "EEPROM.h"
EEPROMMock EEPROM;
#include "../../arduino/medicart_test/medicart_test.ino"
static int fails = 0;
static int L() { return g_pwm[2] ? g_pwm[2] : -g_pwm[3]; }
static int R() { return g_pwm[4] ? g_pwm[4] : -g_pwm[5]; }
static void run(unsigned long ms) { for (unsigned long i = 0; i < ms; i++) { g_ms++; loop(); } }
static void check(bool c, const char* what) { printf("%s %s  (L=%d R=%d)\n", c ? "PASS" : "FAIL", what, L(), R()); if (!c) fails++; }
static void lineAt(int idx, int white, int black) { for (int n = 0; n < 5; n++) g_an[n] = (n == idx) ? black : white; }
int main() {
  setup(); run(300);
  check(Serial.out.find("MediCart TEST ready") != std::string::npos, "boots and prints help");
  check(Serial.out.find("IR 00100 | A  100  100  900  100  100 | pos +0.00 | STOP") != std::string::npos, "prints sensors all the time (line under S3)");
  check(L() == 0 && R() == 0, "starts stopped");
  // manual
  Serial.feed("w\n"); run(50); check(L() == 140 && R() == 140, "w: forward");
  Serial.feed("a\n"); run(20); check(L() == -105 && R() == 105, "a: turn left");
  Serial.feed("d\n"); run(20); check(L() == 105 && R() == -105, "d: turn right");
  Serial.feed("s\n"); run(20); check(L() == -140 && R() == -140, "s: back");
  Serial.out.clear(); run(400); check(Serial.out.find("MAN L=-140 R=-140") != std::string::npos, "sensor printout keeps running while driving");
  run(2700); check(L() == 0 && R() == 0, "manual stops by itself after 3 s");
  Serial.feed("wwww\n"); run(2500); Serial.feed("w\n"); run(2500); check(L() == 140, "typing w again keeps going");
  Serial.feed("x\n"); run(5); check(L() == 0 && R() == 0, "x stops");
  Serial.feed("w"); run(100); check(L() == 140, "works with 'No line ending' too");
  Serial.feed("x"); run(100);
  // calibration: line reads HIGH (typical); slide across: mostly white
  Serial.out.clear(); Serial.feed("c\n"); run(10);
  for (int t = 0; t < 5000; t++) { int phase = (t / 250) % 10; if (phase < 5) lineAt(phase, 120, 880); else lineAt(-1, 120, 880); g_ms++; loop(); }
  run(10);
  check(Serial.out.find("CALIBRATION SAVED") != std::string::npos && Serial.out.find("line reads HIGHER") != std::string::npos, "calibration: learns line reads higher");
  // line following
  lineAt(2, 120, 880); Serial.feed("l\n"); run(50); check(L() == 140 && R() == 140, "LFR: line in the middle -> straight");
  lineAt(3, 120, 880); run(30); check(L() > R() && L() > 0, "LFR: line under S4 (right) -> turns right");
  lineAt(1, 120, 880); run(30); check(R() > L() && R() > 0, "LFR: line under S2 (left) -> turns left");
  lineAt(0, 120, 880); run(30); check(R() > L() + 100, "LFR: line under S1 (far left) -> hard left");
  Serial.out.clear(); run(300); check(Serial.out.find("IR 10000") != std::string::npos && Serial.out.find("LFR") != std::string::npos, "printout runs in parallel with line following");
  lineAt(-1, 120, 880); run(30); check(L() < 0 && R() > 0, "LFR: line lost (last seen left) -> spins left to find it");
  run(1300); check(L() == 0 && R() == 0, "LFR: gives up after 1.2 s and stops");
  lineAt(2, 120, 880); run(30); check(L() == 140, "LFR: line found again -> continues");
  Serial.feed("l\n"); run(5); check(L() == 0 && R() == 0, "l again: line following off");
  // speed / settings
  Serial.feed("SPEED 160\n"); Serial.feed("l\n"); run(30); check(L() == 160, "SPEED 160");
  Serial.feed("x\n"); run(5);
  Serial.feed("+\n"); run(5); check(speedBase == 170, "+ raises speed");
  // restart keeps calibration + speed
  Serial.out.clear(); setup(); run(5); check(Serial.out.find("speed=170") != std::string::npos && Serial.out.find("calibrated=yes") != std::string::npos, "settings + calibration survive a restart");
  // a board where the line reads LOW (white high)
  Serial.feed("c\n"); run(10);
  for (int t = 0; t < 5000; t++) { int phase = (t / 250) % 10; if (phase < 5) lineAt(phase, 950, 60); else lineAt(-1, 950, 60); g_ms++; loop(); }
  run(10);
  check(Serial.out.find("line reads LOWER") != std::string::npos, "calibration: learns line reads lower (other board type)");
  lineAt(3, 950, 60); Serial.feed("l\n"); run(30); check(L() > R(), "LFR on that board: line right -> turns right");
  Serial.feed("x\n"); run(5);
  // digital array, uncalibrated: 0 / 1023
  Serial.feed("RESET\n"); run(5); lineAt(2, 0, 1023); Serial.out.clear(); run(250);
  check(Serial.out.find("IR 00100") != std::string::npos, "digital array without calibration: line = 1023 works");
  Serial.feed("i\n"); run(5); Serial.out.clear(); run(250); check(Serial.out.find("IR 11011") != std::string::npos, "i inverts");
  Serial.feed("p\n"); run(5); Serial.out.clear(); run(1000); check(Serial.out.find("IR ") == std::string::npos, "p pauses the printout");
  printf("\n%s: %d failure(s)\n", fails ? "FAILED" : "ALL PASSED", fails); return fails ? 1 : 0;
}
