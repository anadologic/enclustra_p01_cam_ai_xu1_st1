// p01-app - shared helpers
#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace p01 {

inline double now_ms() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

inline void log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::fprintf(stderr, "[p01] ");
  std::vfprintf(stderr, fmt, ap);
  std::fprintf(stderr, "\n");
  va_end(ap);
}

// Exponential moving average for on-screen statistics.
struct Ema {
  double value = 0.0;
  bool   init  = false;
  void add(double v, double alpha = 0.1) {
    value = init ? value + alpha * (v - value) : v;
    init  = true;
  }
};

}  // namespace p01
