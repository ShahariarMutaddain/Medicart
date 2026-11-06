#include "Arduino.h"
unsigned long g_ms = 0; int g_pwm[64] = {0}; int g_dig[80] = {0}; int g_ir[5] = {1,1,0,1,1}; // black line: LOW = on line -> 00100
SerialMock Serial;
#include "../../arduino/medicart/medicart.ino"
static int fails = 0;
static int L() { return g_pwm[2] ? g_pwm[2] : -g_pwm[3]; }
static int R() { return g_pwm[4] ? g_pwm[4] : -g_pwm[5]; }
static void run(unsigned long ms) { for (unsigned long i = 0; i < ms; i++) { g_ms++; loop(); } }
static void check(bool c, const char* what) { printf("%s %s  (L=%d R=%d)\n", c ? "PASS" : "FAIL", what, L(), R()); if (!c) fails++; }
int main() {
  setup();
  check(Serial.out.find("MediCart ready") != std::string::npos && Serial.out.find("FW medicart-ctrl 2.1") != std::string::npos, "boot banner + firmware id");
  run(50); check(L() == 0 && R() == 0, "boots stopped (no SAFE yet)");
  // ---- AUTO: Rifat's behaviour ----
  for (int i = 0; i < 6; i++) { Serial.feed("SAFE\n"); run(350); }
  check(L() == 180 && R() == 180, "AUTO: SAFE heartbeats -> line following, centre = FULL_SPEED");
  run(2100); check(L() == 0 && R() == 0, "AUTO: heartbeat stops -> watchdog stop within 2 s");
  Serial.feed("SAFE\n"); run(20); check(L() == 180, "AUTO: SAFE resumes");
  Serial.feed("STOP\n"); run(5); check(L() == 0 && R() == 0, "AUTO: STOP stops at once");
  Serial.feed("QR:3\n"); run(5); check(g_dig[8] == HIGH, "AUTO: QR:3 sets pin 8 (unchanged)");
  // ---- MANUAL ----
  Serial.feed("MAN:150,150\n"); run(1); check(Serial.out.find("DBG mode=MANUAL") != std::string::npos, "MAN: enters MANUAL mode");
  for (int i = 0; i < 6; i++) { Serial.feed("MAN:150,150\n"); run(100); }
  check(L() == 150 && R() == 150, "MANUAL: wheels reach requested PWM (ramped)");
  check(g_dig[24] == LOW, "MANUAL: buzzer quiet while driving even without SAFE heartbeats");
  Serial.feed("MAN:255,-255\n"); run(400); check(L() == 200 && R() == -200, "MANUAL: clamped to MANUAL_MAX_PWM, pivot works");
  run(150); check(L() == 0 && R() == 0, "MANUAL: no MAN: for 0.5 s -> wheels stop (dead-man)");
  Serial.feed("MAN:120,120\n"); run(3); check(L() > 0 && L() < 120, "MANUAL: accel ramp (not a jump)");
  for (int i = 0; i < 3; i++) { Serial.feed("MAN:120,120\n"); run(100); }
  Serial.feed("STOP\n"); run(2); check(L() == 0 && R() == 0, "MANUAL: STOP halts immediately");
  Serial.feed("MAN:100,100\n"); run(300); Serial.feed("SAFE\n"); Serial.feed("MAN:100,100\n"); run(100);
  check(L() == 100, "MANUAL: SAFE does not switch back to line following");
  Serial.feed("MAN:abc\n"); Serial.feed("MAN:100,100\n"); run(50); check(L() == 100, "MANUAL: malformed MAN ignored");
  // ---- back to AUTO ----
  Serial.feed("AUTO\n"); run(5); check(L() == 0 && R() == 0 && Serial.out.find("DBG mode=AUTO") != std::string::npos, "AUTO: motors stop on switch");
  run(300); check(L() == 0, "AUTO: stays stopped until the next SAFE");
  Serial.feed("SAFE\n"); run(20); check(L() == 180, "AUTO: next SAFE resumes line following");
  // ---- STAT ----
  Serial.out.clear(); Serial.feed("STAT\n"); run(1);
  check(Serial.out.find("STAT mode=AUTO obstacle=0") != std::string::npos && Serial.out.find("line=00100") != std::string::npos, "STAT line");
  printf("STAT sample: %s", Serial.out.c_str());
  // ---- garbage ----
  for (int i = 0; i < 200; i++) { Serial.feed("x"); }
  Serial.feed("\nSAFE\n"); run(10); check(L() == 180, "long garbage line dropped, SAFE still parsed");
  printf("\n%s: %d failure(s)\n", fails ? "FAILED" : "ALL PASSED", fails);
  return fails ? 1 : 0;
}
