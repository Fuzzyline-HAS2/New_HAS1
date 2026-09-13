// PN532 RxGain 고정. 모듈 로트마다 RF 특성이 달라 기본값(RFCfg 0x59 = 38dB)에서는
// 리더별 인식 대역이 제각각이 된다. 실측(2026-09-13, 리더 3개 x Gain 5종 누적 검출횟수):
//
//        0x19(23dB)  0x29  0x39  0x49(33dB)  0x59(기본)
//   [0]      17       16    18       11         19
//   [1]      11        9     8        0          2     <- 높은 Gain에서 먹통
//   [2]      13       10    10       11          0     <- 기본값에서 먹통
//
// 기본값 0x59는 세 리더 편차가 19/2/0으로 극단적이라 "한 리더만 인식되는" 증상이 났다.
// 0x19는 세 리더 모두 0이 없는 유일한 값이라 이것으로 고정한다.
//
// TX(GsNOn/CWGsP)는 기본값 유지 — 출력만 낮추는 것은 근거리 개선 효과가 없었다.
// (슬랙 2026-08-13 PN532 인식거리 개선 스레드 참고)
//
// 이 설정은 PN532에 영구 저장되지 않으므로 초기화할 때마다 다시 적용해야 한다.
// 근거리/원거리를 모두 덮으려면 0x19 <-> 0x49 교대가 필요하지만, 리더 3개분
// 상태 관리(UID 고정, TAG_REMOVE_TIME 판정)가 필요해 고정으로 충분한지 먼저 확인한다.
static const uint8_t PN532_RXGAIN_23DB = 0x19;

static bool ApplyGain(int idx, uint8_t rfCfg)
{
    uint8_t cmd[] = {
        0x32,       // RFConfiguration
        0x0A,       // Type A 106kbps Analog Setting
        rfCfg,      // RFCfg - RxGain
        0xF4,       // GsNOn      (TX 기본값)
        0x3F,       // CWGsP      (TX 기본값)
        0x11,       // ModGsP
        0x4D,       // Demod RF ON
        0x85,       // RxThreshold
        0x61,       // Demod RF OFF
        0x6F,       // GsNOff
        0x26,       // ModWidth
        0x62,       // MifNFC
        0x87        // TxBitPhase
    };
    return nfc[idx].sendCommandCheckAck(cmd, sizeof(cmd), 1000);
}

void RfidInit()
{
    RestartPn532:
    rfid_init_complete_cnt = 0;
    for (int i = 0; i < rfid_num; ++i)
    {
        nfc[i].begin();
        if (!(nfc[i].getFirmwareVersion()))
        {
            Serial.println("B:PN532_FAIL_" + String(i));
            goto RestartPn532;
        }
        else
        {
            nfc[i].SAMConfig();
            nfc[i].setPassiveActivationRetries(0x01); // 카드 없을 때 내부 재시도(기본값 사실상 무한)로 응답이 지연되는 것을 막아 폴링 속도를 높임
            ApplyGain(i, PN532_RXGAIN_23DB);          // 리더마다 개별 적용 (칩별 설정이라 한 번에 안 됨)
            rfid_init_complete[i] = true;
            rfid_init_complete_cnt++;
        }
        delay(100);
    }
}
void RfidLoopMain(void)
{
  uint8_t uid[3][7] = {{0, 0, 0, 0, 0, 0, 0},
                       {0, 0, 0, 0, 0, 0, 0},
                       {0, 0, 0, 0, 0, 0, 0}}; // Buffer to store the returned UID
  uint8_t uidLength[] = {0};                   // Length of the UID (4 or 7 bytes depending on ISO14443A card type)
  uint8_t data[32];
  uint8_t detectedUid[7];
  uint8_t detectedUidLength;

  for (int i = 0; i < rfid_num; ++i)
  {
    structTagData[i].tagData = "GxP0"; // 매 루프마다 기본값(태그 없음)으로 먼저 초기화 -> 통신 실패/읽기 실패 시에도 이전 판 태그값이 남지 않도록 flush
    if (nfc[i].startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A)){ // InListPassiveTarget 명령이 정상적으로 응답됐는지만 확인 (태그를 찾았는지는 아직 모름)
      if (nfc[i].readDetectedPassiveTargetID(detectedUid, &detectedUidLength)){ // 위 명령의 응답을 실제로 읽어서 태그를 진짜 찾았는지 확인 + 응답 버퍼를 비움(drain). 이걸 안 하면 다음 명령 응답과 뒤섞여 이전에 읽은 태그 데이터가 잘못 재사용될 수 있음
        if (nfc[i].ntag2xx_ReadPage(7, data)){ // ntag 데이터에 접근해서 불러와서 data행열에 저장
            structTagData[i].tagData = "";
            for(int j = 0; j < 4; j++)
                structTagData[i].tagData += (char)data[j];
            if(data[0] == 'M')
            {
              // MMMM은 관리자 카드다. 별도 'M' 명령으로만 알리고, T 패킷의 태그 데이터에는
              // 싣지 않는다. 둘 다 보내면 TTGO가 한 번에 한 줄만 읽는 탓에 'M'이 버려지고
              // "MMMM"이 플레이어 태그로 처리돼(PlayerDetector의 role 미해석 경고 + 서버
              // tagged_players에 MMMM 기록) MMMM 핸들러가 실행되지 않는다. 실측 2026-09-13.
              Serial.println("M");
              structTagData[i].tagData = "GxP0";
            }
        }
      }
    }
  }
  serialSend = true; // 매 루프마다 3개 리더의 최신 상태를 항상 전송 (다른 리더의 새 값과 함께 이전 판의 stale 값이 섞여 전송되는 것을 방지)
}
