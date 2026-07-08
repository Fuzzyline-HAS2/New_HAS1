void RfidInit()
{
  nfc.begin();
  if (!nfc.getFirmwareVersion())
  {
    Log("RFID", "PN532 connect FAIL");
  }
  else
  {
    nfc.SAMConfig();
    Log("RFID", "PN532 online");
    AllNeoOn(YELLOW);
  }
}

/**
 * @brief 태그가 감지되어 NTAG 데이터를 읽었으면 true. 태그 처리(상태 전환)는 Game_system.ino가 담당
 */
bool RfidReadTag(uint8_t data[32])
{
  byte pn532_packetbuffer11[64];
  pn532_packetbuffer11[0] = 0x00;
  if (!nfc.sendCommandCheckAck(pn532_packetbuffer11, 1))  return false;  // rfid 통신 가능한 상태인지 확인
  if (!nfc.startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A)) return false;  // tag 찍혔는지 확인
  return nfc.ntag2xx_ReadPage(7, data);                   // ntag 데이터를 data 배열에 저장
}

/**
 * @brief 태그가 리더 위에 있는지만 확인 (데이터 읽기 없음, 퍼즐 중 태그 유지 확인용)
 */
bool RfidTagPresent()
{
  byte pn532_packetbuffer11[64];
  pn532_packetbuffer11[0] = 0x00;
  if (!nfc.sendCommandCheckAck(pn532_packetbuffer11, 1)) return false;
  return nfc.startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A);
}

/**
 * @brief 태그 데이터에서 사용자 문자열(GxPx) 추출. "MMMM" 관리자 카드는 즉시 재부팅
 */
String CheckingPlayers(uint8_t rfidData[32])
{
  String tagUser = "";
  for (int i = 0; i < 4; i++)                             // GxPx 데이터만 배열에서 추출해서 string으로 저장
    tagUser += (char)rfidData[i];
  Log("RFID", "tag: " + tagUser);

  if (tagUser == "MMMM")
  {                                                       //"MMMM"일경우 관리자 카드 → 즉시 재부팅
    Log("RFID", "admin card -> restart");
    ESP.restart();
  }
  return tagUser;
}
