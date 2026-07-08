void Mp3_Init(){
  MP3Serial.begin(9600);

  Log("MP3", "init start (SD 스캔 대기 2s)");
  delay(2000);  // DFPlayer는 전원 인가 후 SD 스캔에 1.5~3초 필요 — ESP32가 먼저 깨어나므로 기다려야 콜드부팅에서 성공
  myDFPlayer.setTimeOut(1000);

  if (!myDFPlayer.begin(MP3Serial)) {
    dfPlayerReady = false;
    Log("MP3", "init FAIL (배선/SD카드 확인) - 오디오 없이 계속");
    return;  // dfplayer 초기화 실패 시 넘어간다.
  }
  dfPlayerReady = true;
  Log("MP3", "online");
  myDFPlayer.setTimeOut(500);
  myDFPlayer.volume(30); // 볼륨 30으로 설정 (0~30 값 허용)
  myDFPlayer.EQ(DFPLAYER_EQ_ROCK); // EQ 모드 설정
  myDFPlayer.outputDevice(DFPLAYER_DEVICE_SD);
}

void Mp3PlayLargeFolder(uint8_t folder_number, uint16_t file_number)
{
  if(!dfPlayerReady) return;  // DFPlayer 미초기화 시 스킵
  
  myDFPlayer.playLargeFolder(folder_number, file_number);
}