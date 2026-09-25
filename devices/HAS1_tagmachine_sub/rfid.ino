// ── PN532 근접 인식 Dead Zone 대응 — RxGain 동적 전환 ───────────────────────
// 일부 생산 로트의 PN532는 기본 RxGain(38dB)에서 태그를 안테나 중심에 맞춰 대면
// 약 2cm 이하 근거리에서 인식이 안 되는 특성이 있음이 실측으로 확인됨
// (동일 TTGO/배선/태그/펌웨어에서 PN532만 교체해 재현 — 로트별 RF 편차, MCU/통신 문제 아님).
// RxGain을 낮추면(23dB) 근거리(~2cm)가, 기본보다 높이면(33dB) 중거리(2~4cm)가 각각
// 커버되므로, 두 세팅을 상황에 따라 전환해 근접~4cm 전체 구간을 잇는다.
// TX 출력(GsNOn/CWGsP)은 실측상 근거리 개선 기여가 낮아 PN532 기본값을 그대로 둔다.
// (GainMode enum, TAG_REMOVE_TIME_MS 정의는 HAS1_tagmachine_sub.h — Arduino 자동
// 프로토타입 생성 때문에 여기 두면 안 됨)

static GainMode      currentGain    = GAIN_NEAR;
static bool          tagLocked      = false;   // 태그를 찾아 유지 중인지 (탐색 모드 vs 유지 모드)
static uint8_t       lockedData[RFID_TAG_DATA_LENGTH]; // page7의 실제 4바이트 — 동일 태그 판별 기준
static unsigned long lastSeenMs     = 0;         // 유지 중 태그를 마지막으로 확인한 시각

// RFConfiguration(0x32) CfgItem 0x0A(Type A 106kbps Analog Setting)로 RxGain을 전환한다.
// PN532는 이 설정을 내부에 영구 저장하지 않으므로, 초기화/재초기화(모듈 교체 포함) 때마다
// 다시 적용해야 한다.
static bool ApplyGain(int mode) {
    uint8_t rfCfg = (mode == GAIN_NEAR) ? 0x19 : 0x49;  // 23dB(근거리) / 33dB(중거리)
    uint8_t cmd[] = {
        0x32,       // RFConfiguration
        0x0A,       // Type A 106kbps Analog Setting
        rfCfg,      // RFCfg — RxGain (아래 TX 관련 값들은 실측상 기본값 유지가 최선이었음)
        0xF4,       // GsNOn
        0x3F,       // CWGsP
        0x11,       // ModGsP
        0x4D,       // Demod RF ON
        0x85,       // RxThreshold
        0x61,       // Demod RF OFF
        0x6F,       // GsNOff
        0x26,       // ModWidth
        0x62,       // MifNFC
        0x87        // TxBitPhase
    };
    return nfc.sendCommandCheckAck(cmd, sizeof(cmd), 1000);
}

// 현재 칩에 적용된 Gain으로 태그 감지 + page7 읽기를 1회 시도한다.
static bool DetectAndRead(uint8_t outData[RFID_TAG_DATA_LENGTH]) {
    uint8_t uid[7];
    uint8_t uidLength = 0;

    // readPassiveTargetID()는 InListPassiveTarget의 ACK와 결과 프레임을 모두 소비한다.
    // 기존 0x00(정의되지 않은 명령) + startPassiveTargetIDDetection() 조합은 결과를
    // 읽지 않은 채 다음 명령을 보내 PN532 프레임 위상을 깨뜨릴 수 있었다.
    if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength,
                                 RFID_DETECT_TIMEOUT_MS)) {
        return false;
    }
    return nfc.ntag2xx_ReadPage(7, outData) != 0;
}

// page7 데이터 앞 4바이트를 태그 문자열로 뽑아 기존 프로토콜 그대로 전송한다
// (main 보드의 CommnunicationBeetle/CommnunicationMainBeetle이 이 포맷을 기대함).
static void SendTagData(const uint8_t data[RFID_TAG_DATA_LENGTH]) {
    String tagData = "";
    for (int i = 0; i < RFID_TAG_DATA_LENGTH; i++)
        tagData += (char)data[i];
    Serial.println(tagData);
    fromSubSerial.println(tagData);
}

bool RfidInit()
{
    nfc.begin();
    if (!(nfc.getFirmwareVersion()))
    {
        Serial.println("PN532 firmware query failed");
        rfid_init_complete = false;
        return false;
    }

    if (!nfc.SAMConfig()) {
        Serial.println("PN532 SAMConfig failed");
        rfid_init_complete = false;
        return false;
    }

    // 기본 0xFF는 카드가 올 때까지 InListPassiveTarget을 무한 수행한다. 유한 재시도를
    // 설정해야 readPassiveTargetID의 timeout 뒤에도 PN532가 다음 명령을 받을 수 있다.
    if (!nfc.setPassiveActivationRetries(RFID_ACTIVATION_RETRIES)) {
        Serial.println("PN532 activation retry setup failed");
        rfid_init_complete = false;
        return false;
    }

    // PN532는 RF 설정을 저장하지 않으므로 초기화(전원 재투입/모듈 교체 포함)마다 재적용한다.
    currentGain = GAIN_NEAR;
    if (!ApplyGain(currentGain)) {
        Serial.println("PN532 gain setup failed");
        rfid_init_complete = false;
        return false;
    }

    tagLocked = false;
    rfid_init_complete = true;
    Serial.println("PN532 connected");
    return true;
}

// 탐색 모드(태그 미보유): 현재 Gain으로 1회 시도 → 실패하면 반대 Gain으로 즉시 재시도
//   (23dB↔33dB를 오가며 근접~4cm 전 구간을 커버, 둘 다 실패하면 이번 loop tick은 미검출)
// 유지 모드(태그 보유): 현재 Gain으로 먼저 확인 → 실패하면 반대 Gain으로 즉시 재확인
//   → 그래도 둘 다 실패하면 TAG_REMOVE_TIME_MS 동안은 유지로 간주(단발성 미스 무시, 이번 tick은 전송 없음)
//   → 유예시간 초과 시에만 최종적으로 태그 제거 판정, 이후 탐색 모드로 복귀
void RfidLoopMain(void)
{
    uint8_t data[RFID_TAG_DATA_LENGTH];

    if (!tagLocked) {
        if (!DetectAndRead(data)) {
            GainMode otherGain = (currentGain == GAIN_NEAR) ? GAIN_FAR : GAIN_NEAR;
            if (!ApplyGain(otherGain)) {
                Serial.println("PN532 gain command failed; scheduling recovery");
                RequestRfidReinit(false);
                return;
            }
            currentGain = otherGain;
            if (!DetectAndRead(data)) return;   // 두 Gain 모두 미검출 — 이번 tick 스킵
        }
        // 태그 발견 — 지금 이 Gain을 유지하며 락온
        tagLocked = true;
        memcpy(lockedData, data, RFID_TAG_DATA_LENGTH);
        lastSeenMs = millis();
        SendTagData(lockedData);
        return;
    }

    bool found = DetectAndRead(data) &&
                 memcmp(data, lockedData, RFID_TAG_DATA_LENGTH) == 0;
    if (!found) {
        GainMode previousGain = currentGain;
        GainMode otherGain = (currentGain == GAIN_NEAR) ? GAIN_FAR : GAIN_NEAR;
        if (!ApplyGain(otherGain)) {
            Serial.println("PN532 gain command failed; scheduling recovery");
            RequestRfidReinit(false);
            return;
        }
        if (DetectAndRead(data) &&
            memcmp(data, lockedData, RFID_TAG_DATA_LENGTH) == 0) {
            currentGain = otherGain;  // 반대 Gain에서 같은 태그 재확인 → 그 Gain으로 전환해 유지
            found = true;
        } else {
            if (!ApplyGain(previousGain)) {
                Serial.println("PN532 gain restore failed; scheduling recovery");
                RequestRfidReinit(false);
                return;
            }
            currentGain = previousGain;
        }
    }

    if (found) {
        lastSeenMs = millis();
        SendTagData(lockedData);
        return;
    }

    if (millis() - lastSeenMs < TAG_REMOVE_TIME_MS) {
        return;  // 유예시간 이내 — 단발성 미스로 보고 이번 tick은 전송 없이 유지 상태만 지속
    }

    // 유예시간 초과 — 태그 제거 확정, 탐색 모드로 복귀
    tagLocked = false;
    if (!ApplyGain(GAIN_NEAR)) {
        Serial.println("PN532 gain reset failed; scheduling recovery");
        RequestRfidReinit(false);
        return;
    }
    currentGain = GAIN_NEAR;
}
