#include "HAS1_altar.h"

SoftwareSerial MP3Serial(DFPLAYER_RX_PIN, DFPLAYER_TX_PIN);
DFRobotDFPlayerMini myDFPlayer;
bool dfPlayerReady = false;

void Mp3_Init()
{
  MP3Serial.begin(9600);
  delay(2000); // SD카드 스캔 대기

  myDFPlayer.setTimeOut(1000);
  if (myDFPlayer.begin(MP3Serial))
  {
    dfPlayerReady = true;
    myDFPlayer.setTimeOut(500);
    myDFPlayer.volume(30);
    myDFPlayer.EQ(DFPLAYER_EQ_NORMAL);
    myDFPlayer.outputDevice(DFPLAYER_DEVICE_SD);
    Serial.println("DFPlayer connected successfully");
  }
  else
  {
    Serial.println("!!!DFPlayer connect failed!!! - continuing anyway");
  }
}

void Mp3PlayLargeFolder(uint8_t folder_number, uint16_t file_number)
{
  if (!dfPlayerReady) return;
  myDFPlayer.playLargeFolder(folder_number, file_number);
}
