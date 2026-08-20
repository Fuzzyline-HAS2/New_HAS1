// =================================================================================
// beetle_ota.ino
// ---------------------------------------------------------------------------------
// Main/Sub Beetle(ESP32-C3)은 자체 WiFi가 없어 GitHub에서 직접 OTA를 받을 수 없다.
// 그래서 TTGO가 대신 GitHub Release(HAS1_tagmachine_sub 태그)에서 update.bin을
// 다운로드하면서 HMAC-SHA256 서명을 검증하고(SecureOTA.cpp의 _execOTA와 동일한 신뢰
// 모델), 검증에 성공한 바이트만 UART로 그대로 릴레이한다. Beetle은 받은 바이트를
// Update.h(자체 OTA 파티션)에 흘려 넣기만 하면 되므로, Beetle 쪽에는 서명 검증 로직이
// 없다(물리적으로 기기 내부에 유선 연결된 사설 링크이기 때문에 TTGO의 검증을 신뢰).
//
// 프레임 포맷은 HAS1_tagmachine_sub/ota.ino와 반드시 동기화해서 유지할 것.
//   [0xA5][0x5A][TYPE:1][LEN_LO][LEN_HI][PAYLOAD:LEN][CRC16_LO][CRC16_HI]
//   CRC16(CCITT-FALSE, init 0xFFFF)은 TYPE+LEN+PAYLOAD 구간에 대해 계산한다.
// =================================================================================

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "mbedtls/md.h"

#define BEETLE_OTA_SOF1 0xA5
#define BEETLE_OTA_SOF2 0x5A

#define BEETLE_OTA_TYPE_BEGIN 0x01
#define BEETLE_OTA_TYPE_DATA  0x02
#define BEETLE_OTA_TYPE_END   0x03
#define BEETLE_OTA_TYPE_ABORT 0x04
#define BEETLE_OTA_TYPE_ACK   0x10
#define BEETLE_OTA_TYPE_NAK   0x11

#define BEETLE_OTA_MAX_PAYLOAD 512        // HAS1_tagmachine_sub/ota.ino와 반드시 동일해야 함
#define BEETLE_OTA_FRAME_TIMEOUT_MS 5000  // 프레임 응답을 기다리는 최대 시간
#define BEETLE_OTA_MAX_RETRY 3            // 프레임 하나당 재전송 허용 횟수

static uint16_t BeetleOtaCrc16Step(uint16_t crc, uint8_t b) {
  crc ^= (uint16_t)b << 8;
  for (int i = 0; i < 8; i++)
    crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  return crc;
}

static bool BeetleOtaReadBytesTimeout(HardwareSerial &s, uint8_t *buf, size_t len, unsigned long timeoutMs) {
  unsigned long start = millis();
  size_t got = 0;
  while (got < len) {
    if (s.available()) {
      got += s.readBytes(buf + got, len - got);
    } else if (millis() - start > timeoutMs) {
      return false;
    }
  }
  return true;
}

static void BeetleOtaSendFrame(HardwareSerial &s, uint8_t type, const uint8_t *payload, uint16_t len) {
  uint8_t header[3] = { type, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
  uint16_t crc = 0xFFFF;
  for (int i = 0; i < 3; i++) crc = BeetleOtaCrc16Step(crc, header[i]);
  for (uint16_t i = 0; i < len; i++) crc = BeetleOtaCrc16Step(crc, payload[i]);

  s.write(BEETLE_OTA_SOF1);
  s.write(BEETLE_OTA_SOF2);
  s.write(header, 3);
  if (len) s.write(payload, len);
  s.write((uint8_t)(crc & 0xFF));
  s.write((uint8_t)(crc >> 8));
}

// ACK/NAK 응답 프레임만 기대(payload는 버림). 성공 시 true + *ackType에 타입 기록.
static bool BeetleOtaReadReply(HardwareSerial &s, uint8_t *ackType) {
  unsigned long start = millis();
  uint8_t sof1;
  // 리셋 직후 잔여 바이트 등 SOF1 앞의 잡음을 흘려보내며 SOF1을 찾는다.
  while (true) {
    if (s.available()) {
      sof1 = s.read();
      if (sof1 == BEETLE_OTA_SOF1) break;
    } else if (millis() - start > BEETLE_OTA_FRAME_TIMEOUT_MS) {
      return false;
    }
  }

  uint8_t sof2;
  if (!BeetleOtaReadBytesTimeout(s, &sof2, 1, BEETLE_OTA_FRAME_TIMEOUT_MS) || sof2 != BEETLE_OTA_SOF2) return false;

  uint8_t header[3];
  if (!BeetleOtaReadBytesTimeout(s, header, 3, BEETLE_OTA_FRAME_TIMEOUT_MS)) return false;
  uint16_t len = header[1] | ((uint16_t)header[2] << 8);
  if (len > BEETLE_OTA_MAX_PAYLOAD) return false;

  uint8_t payload[BEETLE_OTA_MAX_PAYLOAD];
  if (len && !BeetleOtaReadBytesTimeout(s, payload, len, BEETLE_OTA_FRAME_TIMEOUT_MS)) return false;

  uint8_t crcBytes[2];
  if (!BeetleOtaReadBytesTimeout(s, crcBytes, 2, BEETLE_OTA_FRAME_TIMEOUT_MS)) return false;
  uint16_t recvCrc = crcBytes[0] | ((uint16_t)crcBytes[1] << 8);

  uint16_t crc = 0xFFFF;
  for (int i = 0; i < 3; i++) crc = BeetleOtaCrc16Step(crc, header[i]);
  for (uint16_t i = 0; i < len; i++) crc = BeetleOtaCrc16Step(crc, payload[i]);
  if (crc != recvCrc) return false;

  *ackType = header[0];
  return true;
}

// 프레임을 보내고 ACK을 기다리되, 실패(타임아웃/CRC오류/NAK) 시 최대 BEETLE_OTA_MAX_RETRY번 재전송.
static bool BeetleOtaSendAndWaitAck(HardwareSerial &s, uint8_t type, const uint8_t *payload, uint16_t len) {
  for (int attempt = 0; attempt < BEETLE_OTA_MAX_RETRY; attempt++) {
    BeetleOtaSendFrame(s, type, payload, len);
    uint8_t ackType;
    if (BeetleOtaReadReply(s, &ackType) && ackType == BEETLE_OTA_TYPE_ACK) return true;
    Serial.printf("[BeetleOTA] 프레임(type=0x%02X) 응답 실패 — 재시도 %d/%d\n", type, attempt + 1, BEETLE_OTA_MAX_RETRY);
  }
  return false;
}

// "V\n" 텍스트 명령으로 Beetle의 현재 FIRMWARE_VER을 조회("VER:<n>" 응답 기대).
// OTA 세션 시작 전에만 호출 — 세션 중에는 바이너리 프레이밍만 오간다.
static int QueryBeetleVersion(HardwareSerial &s) {
  while (s.available()) s.read();  // 잔여 바이트 flush
  s.println("V");
  unsigned long start = millis();
  while (millis() - start < 3000) {
    if (s.available()) {
      String line = s.readStringUntil('\n');
      line.trim();
      if (line.startsWith("VER:")) return line.substring(4).toInt();
    }
  }
  return -1;
}

// GitHub에서 version.txt를 읽어 정수로 반환 (SecureOTA::_checkServerVersion과 동일한 방식).
static int CheckBeetleServerVersion(const char *versionUrl) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30000);
  HTTPClient http;
  http.begin(client, String(versionUrl));
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(30000);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[BeetleOTA] 버전 확인 실패 (HTTP %d)\n", httpCode);
    http.end();
    client.stop();
    return -1;
  }
  String versionStr = http.getString();
  versionStr.trim();
  int serverVersion = versionStr.toInt();
  http.end();
  client.stop();
  return serverVersion;
}

// GitHub에서 32바이트 HMAC 서명을 다운로드 (SecureOTA::_downloadSignature와 동일).
static bool DownloadBeetleSignature(uint8_t sig[32], const char *sigUrl) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30000);
  HTTPClient http;
  http.begin(client, String(sigUrl));
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK || http.getSize() != 32) {
    Serial.printf("[BeetleOTA] 서명 다운로드 실패 (HTTP %d)\n", httpCode);
    http.end();
    client.stop();
    return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  size_t got = stream->readBytes(sig, 32);
  http.end();
  client.stop();
  return got == 32;
}

// 실제 릴레이: GitHub에서 update.bin을 스트리밍 다운로드하며 HMAC 계산 + Beetle로 청크 전달.
// 성공하면 OTA_END까지 보내고, Beetle이 재부팅하며 다시 보내는 "W" 핸드셰이크를 기다린다.
static bool RelayBeetleFirmware(HardwareSerial &beetleSerial, const char *label) {
  uint8_t downloadedSig[32];
  Serial.printf("[BeetleOTA:%s] 서명 다운로드 중...\n", label);
  if (!DownloadBeetleSignature(downloadedSig, BEETLE_SIG_URL)) {
    Serial.printf("[BeetleOTA:%s] 서명 다운로드 실패 — 중단\n", label);
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30000);
  HTTPClient http;
  http.begin(client, String(BEETLE_FIRMWARE_URL));
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(30000);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[BeetleOTA:%s] 펌웨어 다운로드 실패 (HTTP %d)\n", label, httpCode);
    http.end();
    client.stop();
    return false;
  }
  int contentLength = http.getSize();
  if (contentLength <= 0 || contentLength > 2000000) {
    Serial.printf("[BeetleOTA:%s] 잘못된 펌웨어 크기: %d bytes\n", label, contentLength);
    http.end();
    client.stop();
    return false;
  }
  Serial.printf("[BeetleOTA:%s] 펌웨어 크기: %d bytes — BEGIN 전송\n", label, contentLength);

  uint8_t beginPayload[4] = {
    (uint8_t)(contentLength & 0xFF), (uint8_t)((contentLength >> 8) & 0xFF),
    (uint8_t)((contentLength >> 16) & 0xFF), (uint8_t)((contentLength >> 24) & 0xFF)
  };
  if (!BeetleOtaSendAndWaitAck(beetleSerial, BEETLE_OTA_TYPE_BEGIN, beginPayload, 4)) {
    Serial.printf("[BeetleOTA:%s] BEGIN 실패(Beetle 응답 없음/공간 부족) — 중단\n", label);
    http.end();
    client.stop();
    return false;
  }

  mbedtls_md_context_t hmacCtx;
  mbedtls_md_init(&hmacCtx);
  mbedtls_md_setup(&hmacCtx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&hmacCtx, (const unsigned char *)HMAC_SECRET, strlen(HMAC_SECRET));

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[BEETLE_OTA_MAX_PAYLOAD];
  int remaining = contentLength;
  bool ok = true;

  while (remaining > 0 && ok) {
    unsigned long t = millis();
    while (stream->available() == 0) {
      if (millis() - t > 5000) { ok = false; break; }
      delay(10);
    }
    if (!ok) break;

    int toRead = min((int)stream->available(), min(remaining, BEETLE_OTA_MAX_PAYLOAD));
    int bytesRead = stream->readBytes(buf, toRead);
    if (bytesRead <= 0) continue;

    mbedtls_md_hmac_update(&hmacCtx, buf, bytesRead);
    if (!BeetleOtaSendAndWaitAck(beetleSerial, BEETLE_OTA_TYPE_DATA, buf, bytesRead)) {
      Serial.printf("[BeetleOTA:%s] DATA 전송 실패 — 중단\n", label);
      ok = false;
      break;
    }
    remaining -= bytesRead;
  }

  uint8_t computedSig[32];
  mbedtls_md_hmac_finish(&hmacCtx, computedSig);
  mbedtls_md_free(&hmacCtx);
  http.end();
  client.stop();

  if (!ok || remaining != 0) {
    Serial.printf("[BeetleOTA:%s] 다운로드/전송 불완전 — Beetle에 ABORT 전송\n", label);
    BeetleOtaSendFrame(beetleSerial, BEETLE_OTA_TYPE_ABORT, nullptr, 0);
    return false;
  }

  if (memcmp(computedSig, downloadedSig, 32) != 0) {
    Serial.printf("[BeetleOTA:%s] 서명 검증 실패! 위조/손상 펌웨어 — ABORT 전송\n", label);
    BeetleOtaSendFrame(beetleSerial, BEETLE_OTA_TYPE_ABORT, nullptr, 0);
    return false;
  }
  Serial.printf("[BeetleOTA:%s] 서명 검증 완료 — END 전송\n", label);

  if (!BeetleOtaSendAndWaitAck(beetleSerial, BEETLE_OTA_TYPE_END, nullptr, 0)) {
    Serial.printf("[BeetleOTA:%s] 최종 커밋 실패\n", label);
    return false;
  }

  Serial.printf("[BeetleOTA:%s] 재부팅 대기 중...\n", label);
  unsigned long start = millis();
  bool rebooted = false;
  while (millis() - start < 15000) {
    if (beetleSerial.available()) {
      String line = beetleSerial.readStringUntil('\n');
      if (line.length() && line[0] == 'W') {
        beetleSerial.println("W");
        rebooted = true;
        break;
      }
    }
  }
  Serial.printf("[BeetleOTA:%s] %s\n", label, rebooted ? "업데이트 완료" : "재부팅 확인 실패(수동 점검 필요)");
  return rebooted;
}

// device_state=="github" 처리 시 wifi.ino에서 ota.check()보다 먼저 호출된다 — TTGO
// 자신의 OTA(ota.check())는 성공 시 곧장 재부팅해버려서, 순서를 바꾸면 Beetle 점검이
// 통째로 스킵될 수 있기 때문. Main/Sub 두 Beetle을 순서대로 점검해, 서버 버전과
// 다르면 각각 업데이트한다.
void CheckAndUpdateBeetles() {
  struct { HardwareSerial &serial; const char *label; } targets[] = {
    { toSubSerial,  "Sub"  },
    { toMainSerial, "Main" },
  };
  for (auto &t : targets) {
    int localVer = QueryBeetleVersion(t.serial);
    if (localVer < 0) {
      Serial.printf("[BeetleOTA:%s] 버전 조회 실패 — 스킵\n", t.label);
      continue;
    }
    int serverVer = CheckBeetleServerVersion(BEETLE_VERSION_URL);
    if (serverVer < 0) {
      Serial.printf("[BeetleOTA:%s] 서버 버전 확인 실패 — 스킵\n", t.label);
      continue;
    }
    if (serverVer == localVer) {
      Serial.printf("[BeetleOTA:%s] 이미 최신 버전 (v%d)\n", t.label, localVer);
      continue;
    }
    Serial.printf("[BeetleOTA:%s] 버전 불일치 (v%d -> v%d) — 업데이트 시작\n", t.label, localVer, serverVer);
    RelayBeetleFirmware(t.serial, t.label);
  }
}
