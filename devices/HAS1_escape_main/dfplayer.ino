//****************************************mp3_setup()****************************************************************
void Mp3_Setup(){
  MP3Serial.begin(9600, SERIAL_8N1, DFPLAYER_RX_PIN, DFPLAYER_TX_PIN);
  Serial.println("DFRobot DFPlayer Mini Demo");
  Serial.println("Initializing DFPlayer ... (May take 3~5 seconds)");
  myDFPlayer.setTimeOut(1000);
  if (!myDFPlayer.begin(MP3Serial)) {
    Serial.println("[DFPlayer] 연결 실패 — SD카드/연결선 확인 필요. 그냥 진행.");
    return;
  }
  Serial.println(F("DFPlayer Mini online."));
  myDFPlayer.setTimeOut(500);
  myDFPlayer.volume(30);       // 0~30
  myDFPlayer.EQ(DFPLAYER_EQ_NORMAL);
  myDFPlayer.outputDevice(DFPLAYER_DEVICE_SD);
}//void MP3_SETUP

void Mp3PlayLargeFolder(uint8_t folder_number, uint16_t file_number)
{
  const char* lang = (const char *)shift_machine["selected_language"];
  if (lang != nullptr && String(lang) == "EN")
  {
    folder_number += 1;
  }
  myDFPlayer.playLargeFolder(folder_number, file_number);
}