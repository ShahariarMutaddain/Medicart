// Minimal host-side Arduino mock for behavioural tests of medicart.ino
#pragma once
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <algorithm>
typedef uint8_t byte;
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
enum { A8 = 62, A9, A10, A11, A12 };
extern unsigned long g_ms; extern int g_pwm[64]; extern int g_dig[80]; extern int g_ir[5];
inline unsigned long millis() { return g_ms; }
inline void pinMode(int, int) {}
inline void analogWrite(int p, int v) { g_pwm[p] = v; }
inline void digitalWrite(int p, int v) { g_dig[p] = v; }
inline int digitalRead(int p) { return (p >= A8 && p <= A12) ? g_ir[p - A8] : 0; }
template<class T, class L, class H> T constrain(T x, L lo, H hi) { return x < lo ? lo : (x > hi ? hi : x); }
using std::min; using std::max;
class String {
 public:
  std::string s;
  String() {} String(const char* c) : s(c) {} String(const std::string& x) : s(x) {}
  void reserve(int) {}
  String& operator+=(char c) { s += c; return *this; }
  String& operator=(const char* c) { s = c; return *this; }
  bool operator==(const char* c) const { return s == c; }
  unsigned length() const { return s.size(); }
  void trim() { size_t a = s.find_first_not_of(" \t\r\n"); size_t b = s.find_last_not_of(" \t\r\n"); s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1); }
  bool startsWith(const char* p) const { return s.rfind(p, 0) == 0; }
  String substring(unsigned a) const { return a > s.size() ? String("") : String(s.substr(a)); }
  String substring(unsigned a, unsigned b) const { return String(s.substr(a, b - a)); }
  int indexOf(char c, unsigned from = 0) const { size_t i = s.find(c, from); return i == std::string::npos ? -1 : (int)i; }
  long toInt() const { return atol(s.c_str()); }
};
struct SerialMock {
  std::deque<char> in; std::string out;
  void begin(long) {}
  int available() { return (int)in.size(); }
  char read() { char c = in.front(); in.pop_front(); return c; }
  void feed(const std::string& x) { for (char c : x) in.push_back(c); }
  template<class T> void print(T v) { out += std::to_string(v); }
  void print(const char* v) { out += v; }
  void print(const String& v) { out += v.s; }
  template<class T> void println(T v) { print(v); out += "\n"; }
  void println() { out += "\n"; }
};
extern SerialMock Serial;
