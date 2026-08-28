
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
    }
}
void RfidLoopMain(void)
{
    uint8_t uid[3][7] = {{0, 0, 0, 0, 0, 0, 0},
                        {0, 0, 0, 0, 0, 0, 0},
                        {0, 0, 0, 0, 0, 0, 0}}; // Buffer to store the returned UID
    uint8_t uidLength[] = {0};                   // Length of the UID (4 or 7 bytes depending on ISO14443A card type)
    uint8_t data[32];
    byte pn532_packetbuffer11[64];
    pn532_packetbuffer11[0] = 0x00;
    if (nfc.sendCommandCheckAck(pn532_packetbuffer11, 1)){ // rfid 통신 가능한 상태인지 확인
        if (nfc.startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A)){                                       // rfid에 tag 찍혔는지 확인용 //데이터 들어오면 uid정보 가져오기
            if (nfc.ntag2xx_ReadPage(7, data)){ // ntag 데이터에 접근해서 불러와서 data행열에 저장
                String tagData = "";
                for(int i = 0; i < 4; i++)
                    tagData += (char)data[i];
                Serial.println(tagData);
                fromSubSerial.println(tagData);
            }
        }
        else{
        }
    }
}
