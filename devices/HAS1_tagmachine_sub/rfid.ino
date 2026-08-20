
// ── PN532 근접 인식 Dead Zone 대응 — RxGain 동적 전환 (HAS1_generator/HAS1_itembox와 동일 대응) ──
// 일부 생산 로트의 PN532는 기본 RxGain(38dB)에서 태그를 안테나 중심에 맞춰 대면
// 약 2cm 이하 근거리에서 인식이 안 되는 특성이 실측으로 확인됨(로트별 RF 편차,
// MCU/통신 문제 아님). RxGain을 낮추면(23dB) 근거리(~2cm)가, 기본보다 높이면(33dB)
// 중거리(2~4cm)가 각각 커버되므로, 감지 실패 시 반대 Gain으로 즉시 한 번 더 시도해
// 근접~4cm 전 구간을 잇는다. TX 출력(GsNOn/CWGsP)은 실측상 기여가 낮아 기본값 유지.
GainMode currentGain = GAIN_NEAR;

// RFConfiguration(0x32) CfgItem 0x0A(Type A 106kbps Analog Setting)로 RxGain을 전환한다.
// PN532는 이 설정을 내부에 영구 저장하지 않으므로 초기화 때마다(RfidInit) 다시 적용해야 한다.
bool ApplyGain(int mode)
{
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

// 태그 유무만 확인(페이지 읽기 없음). 현재 Gain으로 실패하면 반대 Gain으로 즉시 재시도.
// 성공한 Gain은 currentGain에 남아 다음 호출에도 유지된다.
bool DetectTag()
{
  byte pn532_packetbuffer11[64];
  pn532_packetbuffer11[0] = 0x00;
  if (nfc.sendCommandCheckAck(pn532_packetbuffer11, 1) &&
      nfc.startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A))
    return true;

  currentGain = (currentGain == GAIN_NEAR) ? GAIN_FAR : GAIN_NEAR;
  ApplyGain(currentGain);
  pn532_packetbuffer11[0] = 0x00;
  return nfc.sendCommandCheckAck(pn532_packetbuffer11, 1) &&
         nfc.startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A);
}

// 현재 선택된 태그를 명시적으로 release하고 잠깐 대기.
// loop()가 딜레이 없이 도는 상태에서 딱 붙어있는 카드에 바로 재-anticollision을
// 걸면 카드가 이전 트랜잭션에서 안 빠져나온 채로 재선택되어 COLL 에러가 난다.
void ReleaseTag() {
    uint8_t cmd[2] = { PN532_COMMAND_INRELEASE, 0x00 }; // Tg 0 = 전체 릴리즈
    nfc.sendCommandCheckAck(cmd, 2);
    delay(100);
}

void RfidInit()
{
    RestartPn532:
    nfc.begin();
    if (!(nfc.getFirmwareVersion()))
    {
        Serial.print("PN532 연결실패");
        rfid_init_complete = false;
        goto RestartPn532;
    }
    else
    {
        nfc.SAMConfig();
        Serial.print("PN532 연결성공");
        rfid_init_complete = true;
        currentGain = GAIN_NEAR;
        ApplyGain(currentGain);  // PN532는 RF 설정을 저장하지 않으므로 초기화 때마다 재적용
    }
}
void RfidLoopMain(void)
{
    uint8_t data[32];
    // 태그 한 번 = 전송 한 번이 되도록 디바운스. 이게 없으면 카드가 리더에 붙어있는
    // 짧은 순간에도 루프마다(초당 여러 번) 같은 태그를 계속 재전송해서, TTGO->서버
    // 요청이 폭주하고(순간적인 connection refused 등) 태그를 안 했는데도 반복 태그로
    // 보이는 문제가 생긴다. 카드가 완전히 떨어져 DetectTag()가 실패하는 순간 초기화되어,
    // 다음 태그(같은 카드를 다시 대는 것 포함)는 새 이벤트로 정상 처리된다.
    static String lastTagSent = "";
    // 근접 Dead Zone 대응 위해 DetectTag()로 근/원거리 Gain을 자동 전환하며 시도
    if (DetectTag()){ // rfid에 tag 찍혔는지 확인용(내부에서 통신 가능 여부까지 확인)
        if (nfc.ntag2xx_ReadPage(7, data)){ // ntag 데이터에 접근해서 불러와서 data행열에 저장
            String tagData = "";
            for(int i = 0; i < 4; i++)
                tagData += (char)data[i];
            if (tagData != lastTagSent) {
                Serial.println(tagData);
                fromSubSerial.println(tagData);
                lastTagSent = tagData;
            }
        } else {
            fromSubSerial.println("D:READPAGE_FAIL");
        }
        // 카드를 안테나에 딱 붙여두면 계속 강하게 커플링된 채로 남아있어서,
        // loop()에 딜레이가 없는 상태로 바로 다음 InListPassiveTarget을 걸면
        // 아직 release되지 않은 같은 카드에 재-anticollision을 거는 꼴이 되어 COLL이 뜬다.
        // 명시적으로 release하고 살짝 쉬어서 카드가 트랜잭션에서 완전히 빠져나올 시간을 준다.
        ReleaseTag();
    } else {
        lastTagSent = "";  // 태그 없음 확인 — 다음 태그(같은 카드 재태그 포함)를 새 이벤트로 인정
    }
}
