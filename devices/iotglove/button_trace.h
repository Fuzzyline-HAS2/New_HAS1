#pragma once
#include <stdint.h>
namespace iotglove {
// Fixed-memory, single producer. Diagnostic admission never gates gameplay.
class TraceBudget {
 public:
  bool take(uint32_t now, uint32_t limit, uint32_t period) {
    rotate(now, period);
    if (used_ >= limit) { if (suppressed_ != UINT32_MAX) ++suppressed_; return false; }
    ++used_; return true;
  }
  uint32_t takeSuppressed(uint32_t now, uint32_t period) {
    rotate(now, period);
    const uint32_t out = report_; report_ = 0; return out;
  }
 private:
  uint32_t start_ = 0, used_ = 0, suppressed_ = 0, report_ = 0;
  bool started_ = false;
  void rotate(uint32_t now, uint32_t period) {
    if (!started_ || uint32_t(now-start_) >= period) {
      report_ = suppressed_; suppressed_ = 0; used_ = 0; start_ = now; started_ = true;
    }
  }
};
class ButtonTrace {
 public:
  uint32_t press(uint32_t now) {
    if (++id_ == 0) ++id_;  // Zero means no observed press (e.g. held at boot).
    selected_ = budget_.take(now, 4, 10000); return id_;
  }
  uint32_t id() const { return id_; }
  bool selected() const { return selected_; }
  uint32_t suppressed(uint32_t now) { return budget_.takeSuppressed(now, 10000); }
 private:
  uint32_t id_ = 0;
  bool selected_ = false;
  TraceBudget budget_;
};
class MotorTrace {
 public:
  bool observe(uint32_t now, bool motor) {
    if (known_ && motor == previous_) return false;
    known_ = true; previous_ = motor;
    return budget_.take(now, 16, 1000);
  }
  uint32_t suppressed(uint32_t now) { return budget_.takeSuppressed(now, 1000); }
 private:
  bool known_ = false, previous_ = false;
  TraceBudget budget_;
};
}  // namespace iotglove
