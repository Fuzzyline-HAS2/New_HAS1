#include "library_and_pin.h"

SoftwareSerial       MP3Serial(DFPLAYER_RX_PIN, DFPLAYER_TX_PIN);
DFRobotDFPlayerMini  myDFPlayer;
bool                 dfPlayerReady = false;

void Mp3_Init() {
    MP3Serial.begin(9600);
    Log("MP3", "init start (SD 스캔 대기 2s)");
    delay(2000);  // DFPlayer 전원 인가 후 SD 스캔에 1.5~3초 필요 — 지우면 콜드부팅 실패 재발
    myDFPlayer.setTimeOut(1000);
    if (!myDFPlayer.begin(MP3Serial)) {
        dfPlayerReady = false;
        Log("MP3", "init FAIL (배선/SD카드 확인) - 오디오 없이 계속");
        return;
    }
    dfPlayerReady = true;
    Log("MP3", "online");
    myDFPlayer.setTimeOut(500);
    myDFPlayer.volume(30);
    myDFPlayer.EQ(DFPLAYER_EQ_ROCK);
    myDFPlayer.outputDevice(DFPLAYER_DEVICE_SD);
}

void Mp3PlayLargeFolder(uint8_t folder_number, uint16_t file_number) {
    if (!dfPlayerReady) return;
    myDFPlayer.playLargeFolder(folder_number, file_number);
}
