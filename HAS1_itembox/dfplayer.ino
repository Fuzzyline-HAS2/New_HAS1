void Mp3_Init(){
  MP3Serial.begin(9600);

  Serial.println("DFRobot DFPlayer Mini Demo");
  Serial.println("Initializing DFPlayer ... (May take 3~5 seconds)");
  myDFPlayer.setTimeOut(1000);

  if (!myDFPlayer.begin(MP3Serial)) {
    Serial.println("Unable to begin:");
    Serial.println("1.Please recheck the connection!");
    Serial.println("2.Please insert the SD card!");
    dfPlayerReady = false;
    Serial.println("DFPlayer skipped. Continuing without audio.");
    return;  // dfplayer 초기화 실패 시 넘어간다. 
  }
  dfPlayerReady = true;
  Serial.println(F("DFPlayer Mini online."));
  myDFPlayer.setTimeOut(500);
  myDFPlayer.volume(30); // 볼륨 30으로 설정 (0~30 값 허용)
  myDFPlayer.EQ(DFPLAYER_EQ_ROCK); // EQ 모드 설정
  myDFPlayer.outputDevice(DFPLAYER_DEVICE_SD);
}

void Mp3PlayLargeFolder(uint8_t folder_number, uint16_t file_number)
{
  if(!dfPlayerReady) return;  // DFPlayer 미초기화 시 스킵

  // available()은 "재생 가능"이 아니라 "수신 버퍼에 응답이 있는지"라서
  // 재생 조건으로 쓰면 거의 항상 false → 재생이 스킵되는 버그. 바로 재생 명령을 보낸다
  myDFPlayer.playLargeFolder(folder_number, file_number);
}