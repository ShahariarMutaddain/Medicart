// Runs medicart.ino in real time: serial on stdin/stdout, wheel PWM written to $MOTOR_FILE.
#include "../firmware/Arduino.h"
#include <chrono>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
unsigned long g_ms = 0; int g_pwm[64] = {0}; int g_dig[80] = {0}; int g_ir[5] = {1,1,0,1,1};
SerialMock Serial;
#include FIRMWARE
int main() {
  auto t0 = std::chrono::steady_clock::now();
  fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
  const char* mf = getenv("MOTOR_FILE");
  setup();
  int lastL = 9999, lastR = 9999;
  for (;;) {
    g_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    char buf[256]; ssize_t n = read(0, buf, sizeof buf);
    if (n == 0) return 0;
    for (ssize_t i = 0; i < n; i++) Serial.in.push_back(buf[i]);
    loop();
    if (!Serial.out.empty()) { ssize_t w = write(1, Serial.out.data(), Serial.out.size()); (void)w; Serial.out.clear(); }
    int L = g_pwm[2] ? g_pwm[2] : -g_pwm[3], R = g_pwm[4] ? g_pwm[4] : -g_pwm[5];
    if ((L != lastL || R != lastR) && mf) { FILE* f = fopen(mf, "w"); if (f) { fprintf(f, "%d %d %lu\n", L, R, g_ms); fclose(f); } lastL = L; lastR = R; }
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
}
