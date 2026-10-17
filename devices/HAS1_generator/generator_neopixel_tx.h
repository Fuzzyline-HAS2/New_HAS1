#pragma once

#include <stddef.h>
#include <stdint.h>
#include <driver/rmt_tx.h>

// Generator-only WS2812 transport. Adafruit_NeoPixel still owns color/brightness
// buffers; no Adafruit show()/Arduino HAL event-group completion is used here.
// Instances MUST have static lifetime. A timed-out transaction may still read
// symbols_: neither that buffer nor its encoder/channel may be reused or freed.
namespace GeneratorNeo {
class Sender {
 public:
  static constexpr size_t kMaxPixels = 28;
  static constexpr size_t kMaxBytes = kMaxPixels * 3;
  static constexpr int kWaitMs = 100;

  Sender() = default;
  Sender(const Sender&) = delete;
  Sender& operator=(const Sender&) = delete;

  bool begin(int pin) {
    if (attempted_) return ready_;
    attempted_ = true;  // At most one allocation attempt per strip per boot.
    rmt_copy_encoder_config_t encoderConfig = {};
    esp_err_t err = rmt_new_copy_encoder(&encoderConfig, &encoder_);
    if (err != ESP_OK) return fail(err);
    rmt_tx_channel_config_t config = {};
    config.gpio_num = static_cast<gpio_num_t>(pin);
    config.clk_src = RMT_CLK_SRC_DEFAULT;
    config.resolution_hz = 10000000;
    config.mem_block_symbols = 64;
    config.trans_queue_depth = 1;
    err = rmt_new_tx_channel(&config, &channel_);
    if (err != ESP_OK) return fail(err);
    err = rmt_enable(channel_);
    if (err != ESP_OK) return fail(err);
    ready_ = true;
    return true;
  }

  bool show(const uint8_t* bytes, size_t count) {
    if (!ready_) return false;
    if (!bytes || !count || count > kMaxBytes) return fail(ESP_ERR_INVALID_ARG);

    // A low preamble guarantees reset even for the first frame; the low suffix
    // latches this frame before returning, including back-to-back updates.
    resetSymbol(symbols_[0]);
    size_t next = 1;
    for (size_t b = 0; b < count; ++b) {
      for (uint8_t mask = 0x80; mask; mask >>= 1) {
        rmt_symbol_word_t& symbol = symbols_[next++];
        const bool one = (bytes[b] & mask) != 0;
        symbol.level0 = 1;
        symbol.duration0 = one ? 8 : 4;
        symbol.level1 = 0;
        symbol.duration1 = one ? 4 : 8;
      }
    }
    resetSymbol(symbols_[next++]);
    rmt_transmit_config_t config = {};
    config.flags.eot_level = 0;
    config.flags.queue_nonblocking = 1;
    esp_err_t err = rmt_transmit(channel_, encoder_, symbols_,
                                 next * sizeof(symbols_[0]), &config);
    if (err != ESP_OK) return fail(err);
    err = rmt_tx_wait_all_done(channel_, kWaitMs);
    if (err != ESP_OK) return fail(err);
    return true;
  }

  esp_err_t error() const { return error_; }

 private:
  static void resetSymbol(rmt_symbol_word_t& symbol) {
    symbol.level0 = 0;
    symbol.duration0 = 1500;
    symbol.level1 = 0;
    symbol.duration1 = 1500;  // 300 us at 10 MHz.
  }
  bool fail(esp_err_t err) {
    error_ = err;
    ready_ = false;
    // Deliberately quarantine until reboot, even if a late IRQ arrives. On
    // classic ESP32, rmt_disable() itself can poll TX_DONE without a deadline.
    // Keep all handles/payload alive; do not disable/delete/reset or resubmit.
    return false;
  }

  bool attempted_ = false;
  bool ready_ = false;
  esp_err_t error_ = ESP_OK;
  rmt_channel_handle_t channel_ = nullptr;
  rmt_encoder_handle_t encoder_ = nullptr;
  rmt_symbol_word_t symbols_[kMaxBytes * 8 + 2] = {};
};
}  // namespace GeneratorNeo
