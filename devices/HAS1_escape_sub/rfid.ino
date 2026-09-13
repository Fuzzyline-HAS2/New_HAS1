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
              Serial.println("M");
            }
        }
      }
    }
  }
  serialSend = true; // 매 루프마다 3개 리더의 최신 상태를 항상 전송 (다른 리더의 새 값과 함께 이전 판의 stale 값이 섞여 전송되는 것을 방지)
}
