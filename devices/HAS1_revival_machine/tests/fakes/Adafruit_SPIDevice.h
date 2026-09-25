#pragma once
#include <stddef.h>
#include <stdint.h>
#define SPI_BITORDER_LSBFIRST 0
#define SPI_MODE0 0
// The test defines these at the byte-transfer boundary. Production frame and
// deadline logic remains compiled from the actual firmware source.
bool TestSpiBegin();
void TestSpiTransactionBegin();
void TestSpiTransactionEnd();
uint8_t TestSpiTransfer(uint8_t);
bool TestSpiWrite(const uint8_t*, size_t, const uint8_t*, size_t);
bool TestSpiWriteRead(const uint8_t*, size_t, uint8_t*, size_t);
class Adafruit_SPIDevice {
 public:
  template <typename... Args> explicit Adafruit_SPIDevice(Args...) {}
  bool begin() { return TestSpiBegin(); }
  void beginTransactionWithAssertingCS() { TestSpiTransactionBegin(); }
  void endTransactionWithDeassertingCS() { TestSpiTransactionEnd(); }
  uint8_t transfer(uint8_t value) { return TestSpiTransfer(value); }
  bool write(const uint8_t* data, size_t size, const uint8_t* prefix = nullptr,
             size_t prefixSize = 0) {
    return TestSpiWrite(data, size, prefix, prefixSize);
  }
  bool write_then_read(const uint8_t* data, size_t size, uint8_t* output,
                       size_t outputSize, uint8_t = 0xFF) {
    return TestSpiWriteRead(data, size, output, outputSize);
  }
};
