// Host test of the oximeter engine with a simulated module (PPG packets + result packets).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <random>
#include "engine_extract.h"

static int fails = 0;
static void check(bool c, const char* what, const oxi::Output& o) {
  printf("%s %-62s hr=%3d spo2=%3d finger=%d motion=%d held=%d prog=%d idx=%d link=%d\n", c ? "PASS" : "FAIL", what,
         o.hr, o.spo2, o.finger, o.motion, o.spo2Held, o.progress, o.spo2Index, o.link);
  if (!c) fails++;
}

struct Sim {
  oxi::Engine e;
  uint32_t t = 0;                 // ms
  int fs = 50;                    // samples per second
  double hr = 92, phase = 0;
  int spo2 = 97;
  double amp = 40, dc = 2000;
  bool finger = false, motion = false, spo2Bad = false, sending = true;
  int glitchMs = 0;
  std::mt19937 rng{42};
  double motionOff = 0, motionVel = 0;
  uint32_t nextResult = 0;

  void packet(uint8_t type, const uint8_t* b11) {
    e.feed(0x41, t); e.feed(0x42, t); e.feed(type, t);
    for (int i = 0; i < 11; i++) e.feed(b11[i], t);
  }
  int ppgValue() {
    if (!finger || glitchMs > 0) return 0;
    double p = phase - floor(phase);
    double pulse = exp(-pow((p - 0.15) / 0.07, 2)) + 0.35 * exp(-pow((p - 0.45) / 0.08, 2));
    std::normal_distribution<double> n(0, 2.0);
    double v = dc + amp * pulse + n(rng);
    if (motion) {
      std::normal_distribution<double> m(0, 60.0);
      motionVel = 0.9 * motionVel + m(rng);
      motionOff = 0.95 * motionOff + motionVel;
      if (motionOff > 400) motionOff = 400; if (motionOff < -400) motionOff = -400;
      v += motionOff;
    } else { motionOff *= 0.9; motionVel = 0; v += motionOff; }
    if (v < 0) v = 0;
    return (int)v;
  }
  void run(uint32_t ms) {
    uint32_t end = t + ms;
    while (t < end) {
      uint32_t dt = 1000 / fs;
      t += dt;
      phase += hr / 60.0 * dt / 1000.0;
      if (glitchMs > 0) glitchMs -= dt;
      if (sending) {
        int v = ppgValue();
        uint8_t b[11] = {0};
        b[1] = v & 0xFF; b[2] = (v >> 8) & 0xFF;      // body[2], body[3]
        packet(0x01, b);
        if (t >= nextResult) {
          nextResult = t + 1000;
          uint8_t r[11] = {0};
          // body[1]=status, body[2]=HR (in 88..99 on purpose), body[3]=0x5A, body[4]=SpO2
          r[0] = 0x00; r[1] = finger ? (uint8_t)hr : 0; r[2] = 0x10; r[3] = finger ? (spo2Bad ? 0x7F : spo2) : 0x7F;
          packet(0x02, r);
        }
      }
      e.tick(t);
    }
  }
  oxi::Output out() { return e.out(t); }
};

int main() {
  Sim s;
  s.run(3000);
  check(s.out().link && !s.out().finger && s.out().hr == 0, "no finger: link ok, nothing shown", s.out());
  s.finger = true; s.run(15000);
  auto o = s.out();
  check(o.finger && abs(o.hr - 92) <= 2, "finger 15 s: HR 92 (HR byte also sits in 88..99)", o);
  check(o.spo2 == 97, "SpO2 97", o);
  check(o.spo2Index == 4, "learned the SpO2 byte (body[4]), not the HR byte", o);
  check(o.progress == oxi::LOCK_SECONDS, "locked after steady seconds", o);

  // movement: big swings, and the module stops giving SpO2 (the reported problem)
  s.motion = true; s.spo2Bad = true; s.run(1500);
  o = s.out();
  check(o.finger && o.motion, "moving: finger kept, movement detected", o);
  check(o.spo2 == 97 && abs(o.hr - 92) <= 2, "moving: last good HR and SpO2 held", o);
  s.run(3000);
  o = s.out();
  check(o.spo2 == 97 && o.spo2Held, "4.5 s of movement: SpO2 still held (flagged held)", o);
  s.motion = false; s.run(2000); s.spo2Bad = false; s.run(8000);
  o = s.out();
  check(!o.motion && abs(o.hr - 92) <= 2 && o.spo2 == 97 && !o.spo2Held, "after movement: recovers, fresh values", o);
  check(o.progress == oxi::LOCK_SECONDS, "locked again", o);

  // short glitch (sensor slips for 100 ms)
  s.glitchMs = 100; s.run(200);
  o = s.out();
  check(o.finger && o.spo2 == 97 && o.hr > 0, "100 ms drop-out: reading NOT reset", o);
  s.run(5000);

  // heart rate changes
  s.hr = 70; s.run(12000);
  o = s.out();
  check(abs(o.hr - 70) <= 2, "HR changes 92 → 70: follows", o);
  s.spo2 = 93; s.run(5000);
  o = s.out();
  check(o.spo2 == 93, "SpO2 changes 97 → 93: follows", o);
  s.spo2 = 85; s.run(5000);
  o = s.out();
  check(o.spo2 == 85, "SpO2 85 (below the old 88 limit) is shown", o);

  // long movement with the module still giving SpO2: hold beyond 10 s expires
  s.spo2Bad = true; s.motion = true; s.run(12000);
  o = s.out();
  check(o.finger && o.spo2 == 0, "12 s movement + no SpO2 from module: SpO2 -- (hold expired)", o);
  s.motion = false; s.spo2Bad = false; s.run(10000);
  o = s.out();
  check(o.spo2 == 85 && abs(o.hr - 70) <= 2, "after that: back to normal", o);

  // finger removed
  s.finger = false; s.run(1000);
  o = s.out();
  check(!o.finger && o.hr == 0 && o.spo2 == 0 && o.progress == 0, "finger removed: cleared", o);

  // module unplugged
  s.sending = false; s.run(3000);
  o = s.out();
  check(!o.link, "no packets 3 s: module not connected", o);

  // 100 Hz module, HR 60, learning from flash (index known)
  Sim f; f.fs = 100; f.hr = 60; f.e.setIndex(4); f.finger = true; f.run(15000);
  o = f.out();
  check(abs(o.hr - 60) <= 2 && o.spo2 == 97, "100 Hz module, HR 60, saved SpO2 byte", o);

  // HR 120, SpO2 99
  Sim g; g.hr = 120; g.spo2 = 99; g.finger = true; g.run(15000);
  o = g.out();
  check(abs(o.hr - 120) <= 3 && o.spo2 == 99, "HR 120 SpO2 99", o);

  printf(fails ? "FAILED %d\n" : "ALL PASS\n", fails);
  return fails ? 1 : 0;
}
