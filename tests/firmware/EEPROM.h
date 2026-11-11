#pragma once
#include <cstring>
// host mock of the AVR EEPROM library (survives a simulated reboot)
struct EEPROMMock { unsigned char m[4096]; EEPROMMock() { for (auto& b : m) b = 0xFF; }
  unsigned char read(int a) { return m[a]; } void update(int a, unsigned char v) { m[a] = v; } void write(int a, unsigned char v) { m[a] = v; }
  template<class T> void get(int a, T& t) { memcpy(&t, m + a, sizeof(T)); }
  template<class T> void put(int a, const T& t) { memcpy(m + a, &t, sizeof(T)); } };
extern EEPROMMock EEPROM;
