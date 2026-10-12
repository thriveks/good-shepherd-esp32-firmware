#pragma once
#include <stdint.h>
struct ThermalStreamPolicy {
  uint32_t until = 0;
  bool leased = false;
  void renew(uint32_t now) { until = now + 15000; leased = true; }
  void cancel() { leased = false; }
  bool active(uint32_t now) const { return leased && int32_t(until - now) > 0; }
};
