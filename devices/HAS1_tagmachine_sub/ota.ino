// =================================================================================
// ota.ino
// ---------------------------------------------------------------------------------
// TTGO 메인보드가 UART로 릴레이하는 펌웨어를 받아 Update.h(ESP32 OTA 파티션)로
// 흘려 넣는 수신부. Beetle은 자체 WiFi가 없어 GitHub에서 직접 받을 수 없으므로,
// TTGO가 GitHub Release(HAS1_tagmachine_sub 태그)에서 다운로드 + HMAC-SHA256 서명
// 검증까지 마친 바이트를 그대로 이 UART로 흘려보낸다 — 여기서는 진위를 다시 검증하지
// 않고 TTGO를 신뢰한다(둘 다 기기 내부에 유선으로만 연결된 사설 링크이기 때문).
//
// 프레임 포맷(리틀엔디안), TTGO<->Beetle 양쪽 동일:
//   [0xA5][0x5A][TYPE:1][LEN_LO][LEN_HI][PAYLOAD:LEN][CRC16_LO][CRC16_HI]
//   CRC16(CCITT-FALSE, init 0xFFFF)은 TYPE+LEN+PAYLOAD 구간에 대해 계산한다.
// 반드시 HAS1_tagmachine_main/beetle_ota.ino의 정의와 동기화해서 유지할 것.
//
// 0xA5는 평소 오가는 텍스트 명령('W'/'R'/'V'/'D:'.../태그데이터)의 첫 글자로는 절대
// 나오지 않는 값이라, HAS1_tagmachine_sub.ino의 loop()에서 peek()만으로 안전하게
// OTA 세션 진입 여부를 판별할 수 있다.
//
// SOF/TYPE/크기 상수는 HAS1_tagmachine_sub.h에 정의되어 있다(이유는 그 헤더의 주석 참고).
// =================================================================================

static uint16_t OtaCrc16Step(uint16_t crc, uint8_t b) {
  crc ^= (uint16_t)b << 8;
  for (int i = 0; i < 8; i++)
    crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  return crc;
}

// timeoutMs 안에 len바이트를 다 못 채우면 false. TTGO 연결이 도중에 끊겨도
// 여기서 무한 대기하지 않도록 모든 프레임 읽기가 이 함수를 거친다.
static bool OtaReadBytesTimeout(uint8_t *buf, size_t len, unsigned long timeoutMs) {
  unsigned long start = millis();
  size_t got = 0;
  while (got < len) {
    if (fromSubSerial.available()) {
      got += fromSubSerial.readBytes(buf + got, len - got);
    } else if (millis() - start > timeoutMs) {
      return false;
    }
  }
  return true;
}

static void OtaSendFrame(uint8_t type, const uint8_t *payload, uint16_t len) {
  uint8_t header[3] = { type, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
  uint16_t crc = 0xFFFF;
  for (int i = 0; i < 3; i++) crc = OtaCrc16Step(crc, header[i]);
  for (uint16_t i = 0; i < len; i++) crc = OtaCrc16Step(crc, payload[i]);

  fromSubSerial.write(OTA_SOF1);
  fromSubSerial.write(OTA_SOF2);
  fromSubSerial.write(header, 3);
  if (len) fromSubSerial.write(payload, len);
  fromSubSerial.write((uint8_t)(crc & 0xFF));
  fromSubSerial.write((uint8_t)(crc >> 8));
}

static void OtaSendAck() { OtaSendFrame(OTA_TYPE_ACK, nullptr, 0); }
static void OtaSendNak(uint8_t reason) { OtaSendFrame(OTA_TYPE_NAK, &reason, 1); }

// SOF1은 이미 소비된 상태로 호출됨(loop()에서 peek로 확인 후 read). SOF2부터 CRC까지
// 읽어 검증하고, payload는 outBuf(최소 OTA_MAX_PAYLOAD바이트)에 채운다.
static bool OtaReadFrame(uint8_t *type, uint8_t *outBuf, uint16_t *outLen) {
  uint8_t sof2;
  if (!OtaReadBytesTimeout(&sof2, 1, OTA_FRAME_TIMEOUT_MS) || sof2 != OTA_SOF2) return false;

  uint8_t header[3];
  if (!OtaReadBytesTimeout(header, 3, OTA_FRAME_TIMEOUT_MS)) return false;
  uint16_t len = header[1] | ((uint16_t)header[2] << 8);
  if (len > OTA_MAX_PAYLOAD) return false;

  if (len && !OtaReadBytesTimeout(outBuf, len, OTA_FRAME_TIMEOUT_MS)) return false;

  uint8_t crcBytes[2];
  if (!OtaReadBytesTimeout(crcBytes, 2, OTA_FRAME_TIMEOUT_MS)) return false;
  uint16_t recvCrc = crcBytes[0] | ((uint16_t)crcBytes[1] << 8);

  uint16_t crc = 0xFFFF;
  for (int i = 0; i < 3; i++) crc = OtaCrc16Step(crc, header[i]);
  for (uint16_t i = 0; i < len; i++) crc = OtaCrc16Step(crc, outBuf[i]);
  if (crc != recvCrc) return false;

  *type = header[0];
  *outLen = len;
  return true;
}

// TTGO가 0xA5 프리앰블을 보내면 loop()에서 이 함수로 진입한다(SOF1은 이미 소비됨).
// BEGIN으로 Update.begin() 하고, DATA를 받는 대로 Update.write(), END에서 커밋 후 재부팅.
// 도중 타임아웃/CRC 오류/Update 실패가 나면 Update.abort() 후 조용히 리턴해 기존 앱(RfidLoopMain
// 루프)으로 복귀한다 — 실패했다고 재부팅하면 오히려 멀쩡한 구 펌웨어까지 흔들 이유가 없다.
void HandleOtaSession() {
  uint8_t buf[OTA_MAX_PAYLOAD];
  uint8_t type;
  uint16_t len;

  if (!OtaReadFrame(&type, buf, &len) || type != OTA_TYPE_BEGIN || len != 4) {
    return;  // BEGIN이 아니면 오탐지로 보고 조용히 무시
  }
  uint32_t totalSize = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                        ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);

  if (!Update.begin(totalSize)) {
    Serial.println("[OTA] Update.begin 실패 (파티션 공간 부족?)");
    OtaSendNak(1);
    return;
  }
  OtaSendAck();
  Serial.printf("[OTA] 수신 시작: %u bytes\n", (unsigned int)totalSize);

  uint32_t written = 0;
  while (true) {
    if (!OtaReadFrame(&type, buf, &len)) {
      Serial.println("[OTA] 프레임 수신 타임아웃/오류 — 중단");
      Update.abort();
      return;
    }
    if (type == OTA_TYPE_DATA) {
      size_t w = Update.write(buf, len);
      if (w != len) {
        Serial.println("[OTA] Update.write 실패");
        OtaSendNak(2);
        Update.abort();
        return;
      }
      written += len;
      OtaSendAck();
    }
    else if (type == OTA_TYPE_END) {
      if (written != totalSize || !Update.end(true) || !Update.isFinished()) {
        Serial.println("[OTA] 최종 커밋 실패");
        OtaSendNak(3);
        Update.abort();
        return;
      }
      OtaSendAck();
      Serial.println("[OTA] 완료 — 재부팅");
      delay(500);
      ESP.restart();
    }
    else if (type == OTA_TYPE_ABORT) {
      Serial.println("[OTA] TTGO 요청으로 중단");
      Update.abort();
      return;
    }
    else {
      OtaSendNak(0);
    }
  }
}
