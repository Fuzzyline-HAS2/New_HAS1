#ifndef GENERATOR_TELNET_LOG_BUFFER_H
#define GENERATOR_TELNET_LOG_BUFFER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// A byte history, not a queue of String objects. Callers serialize access.
// Absolute positions allow the sender to detect overwritten bytes explicitly.
template <size_t Capacity> class TelnetLogBuffer {
 public:
  static_assert(Capacity > 0, "Log history must not be empty");
  TelnetLogBuffer() : next_(0) {}
  uint64_t end() const { return next_; }
  uint64_t oldest() const { return next_ > Capacity ? next_ - Capacity : 0; }
  void append(const uint8_t *data, size_t size) {
    if (!data || !size) return;
    if (size > Capacity) {
      const size_t skip = size - Capacity;
      next_ += skip;
      data += skip;
      size = Capacity;
    }
    const size_t start = static_cast<size_t>(next_ % Capacity);
    const size_t first = size < Capacity - start ? size : Capacity - start;
    memcpy(bytes_ + start, data, first);
    memcpy(bytes_, data + first, size - first);
    next_ += size;
  }
  size_t copy(uint64_t position, uint8_t *output, size_t limit) const {
    if (position < oldest() || position >= next_ || !limit) return 0;
    const uint64_t available = next_ - position;
    const size_t size = available < limit ? static_cast<size_t>(available) : limit;
    const size_t start = static_cast<size_t>(position % Capacity);
    const size_t first = size < Capacity - start ? size : Capacity - start;
    memcpy(output, bytes_ + start, first);
    memcpy(output + first, bytes_, size - first);
    return size;
  }
 private:
  uint8_t bytes_[Capacity];
  uint64_t next_;
};

#endif
