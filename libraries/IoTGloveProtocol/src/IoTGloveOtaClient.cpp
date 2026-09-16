#include "IoTGloveOtaClient.h"
#include "IoTGloveOta.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <mbedtls/md.h>
#include <esp_ota_ops.h>

namespace iotglove {
namespace ota {
namespace {
constexpr uint32_t kIdleTimeoutMs = 5000;
constexpr uint32_t kTransferTimeoutMs = 120000;

class Download {
 public:
  ~Download() { http.end(); client.stop(); }
  bool begin(const char* url) {
    // Same trust model/key as SecureOTA: authenticate BOTH metadata and image
    // with HMAC; do not rely on TLS or an unsigned version.txt for authenticity.
    client.setInsecure();
    client.setHandshakeTimeout(8);  // Seconds, unlike HTTPClient's millisecond timeout.
    if (!http.begin(client, url)) return false;
    http.setConnectTimeout(5000);
    http.setTimeout(kIdleTimeoutMs);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setRedirectLimit(4);
    if (http.GET() != HTTP_CODE_OK || http.getSize() <= 0) return false;
    stream = http.getStreamPtr();
    if (!stream) return false;
    stream->setTimeout(kIdleTimeoutMs);
    return true;
  }
  HTTPClient http;
  NetworkClient* stream = nullptr;
 private:
  WiFiClientSecure client;
};

bool smallAsset(const char* url, uint8_t* buffer, size_t capacity, size_t& length) {
  Download download;
  if (!download.begin(url)) return false;
  const int size = download.http.getSize();
  if (size <= 0 || static_cast<size_t>(size) > capacity) return false;
  length = 0;
  const uint32_t started = millis();
  uint32_t progressed = started;
  while (length < static_cast<size_t>(size)) {
    const uint32_t now = millis();
    if (uint32_t(now - started) >= 10000 || uint32_t(now - progressed) >= kIdleTimeoutMs) return false;
    const int available = download.stream->available();
    if (available <= 0) {
      if (!download.http.connected()) return false;
      delay(2); continue;
    }
    size_t count = static_cast<size_t>(available);
    if (count > static_cast<size_t>(size) - length) count = static_cast<size_t>(size) - length;
    const int received = download.stream->read(buffer + length, count);
    if (received <= 0) { delay(2); continue; }
    length += static_cast<size_t>(received);
    progressed = millis();
  }
  return true;
}

bool metadata(const char* base, const char* secret, Metadata& result) {
  char url[176];
  uint8_t bytes[kMetadataCapacity], signature[32], calculated[32];
  size_t length = 0, signatureLength = 0;
  snprintf(url, sizeof(url), "%sota.txt", base);
  if (!smallAsset(url, bytes, sizeof(bytes) - 1, length)) return false;
  snprintf(url, sizeof(url), "%sota.sig", base);
  if (!smallAsset(url, signature, sizeof(signature), signatureLength) || signatureLength != 32) return false;
  const auto* algorithm = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!algorithm || mbedtls_md_hmac(algorithm, reinterpret_cast<const uint8_t*>(secret),
      strlen(secret), bytes, length, calculated) != 0 || !equalHmac(calculated, signature)) return false;
  return parseMetadata(reinterpret_cast<const char*>(bytes), length, result);
}

bool install(const char* base, const Metadata& release, const char* secret) {
  char url[176];
  snprintf(url, sizeof(url), "%supdate.bin", base);
  Download download;
  if (!download.begin(url)) return false;
  const int contentLength = download.http.getSize();
  const esp_partition_t* slot = esp_ota_get_next_update_partition(nullptr);
  if (!slot || contentLength <= 0 || static_cast<size_t>(contentLength) > slot->size) return false;

  mbedtls_md_context_t context;
  mbedtls_md_init(&context);
  const auto* algorithm = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  bool ok = algorithm && mbedtls_md_setup(&context, algorithm, 1) == 0 &&
      mbedtls_md_hmac_starts(&context, reinterpret_cast<const uint8_t*>(secret), strlen(secret)) == 0;
  if (!ok || !Update.begin(static_cast<size_t>(contentLength))) {
    mbedtls_md_free(&context);
    return false;
  }

  uint8_t buffer[1024], calculated[32];
  size_t remaining = static_cast<size_t>(contentLength);
  const uint32_t started = millis();
  uint32_t progressed = started;
  while (ok && remaining) {
    const uint32_t now = millis();
    if (uint32_t(now - started) >= kTransferTimeoutMs || uint32_t(now - progressed) >= kIdleTimeoutMs) {
      ok = false; break;
    }
    const int available = download.stream->available();
    if (available <= 0) {
      if (!download.http.connected()) { ok = false; break; }
      delay(2); continue;
    }
    size_t count = static_cast<size_t>(available);
    if (count > sizeof(buffer)) count = sizeof(buffer);
    if (count > remaining) count = remaining;
    const int received = download.stream->read(buffer, count);
    if (received <= 0) { delay(2); continue; }
    count = static_cast<size_t>(received);
    ok = mbedtls_md_hmac_update(&context, buffer, count) == 0 && Update.write(buffer, count) == count;
    remaining -= count;
    progressed = millis();
    delay(1);  // Keep the main UART/WDT loop schedulable during flash/download.
  }
  if (ok && !remaining)
    ok = mbedtls_md_hmac_finish(&context, calculated) == 0 && equalHmac(calculated, release.imageHmac);
  else ok = false;
  mbedtls_md_free(&context);
  // Commit only the COMPLETE image whose HMAC was authenticated by ota.txt.
  if (ok) ok = Update.end(false) && Update.isFinished();
  if (!ok) Update.abort();
  return ok;
}
}  // namespace

CheckResult checkPinned(const char* board, uint32_t targetVersion, uint32_t currentVersion,
                        uint32_t currentPartition, const char* secret,
                        std::function<void()> onSuccess) {
#if defined(IOTGLOVE_COMPILE_ONLY) && IOTGLOVE_COMPILE_ONLY
  (void)board; (void)targetVersion; (void)currentVersion; (void)currentPartition;
  (void)secret; (void)onSuccess;
  return CheckResult::Failed;
#else
  char base[152];
  if (!secret || !*secret || strstr(secret, "COMPILE_ONLY") || strstr(secret, "PLACEHOLDER") ||
      !strncmp(secret, "REPLACE_WITH_", 13) || WiFi.status() != WL_CONNECTED ||
      !archiveBaseUrl(board, targetVersion, base, sizeof(base))) return CheckResult::Failed;
  Metadata release;
  if (!metadata(base, secret, release) || !matchesMetadata(release, board, targetVersion, currentPartition)) {
    Serial.println("[OTA] Requested release metadata unavailable, invalid or incompatible");
    return CheckResult::Failed;
  }
  if (currentVersion == targetVersion) return CheckResult::Skipped;
  Serial.printf("[OTA] %s: %lu -> %lu\n", board, static_cast<unsigned long>(currentVersion),
                static_cast<unsigned long>(targetVersion));
  if (!install(base, release, secret)) {
    Serial.println("[OTA] Requested image download or verification failed");
    return CheckResult::Failed;
  }
  if (onSuccess) onSuccess();
  delay(100);
  ESP.restart();
  return CheckResult::Failed;  // No successful result before the new image boots.
#endif
}

}  // namespace ota
}  // namespace iotglove
#endif
