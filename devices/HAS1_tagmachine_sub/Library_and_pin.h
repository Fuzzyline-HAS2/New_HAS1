#ifndef _LIBRARY_AND_PIN_
#define _LIBRARY_AND_PIN_

#include <Arduino.h>

#include <Adafruit_PN532.h>
#include <HardwareSerial.h>
#include <SimpleTimer.h>
#include <Update.h>  // TTGO가 UART로 릴레이하는 펌웨어를 자체 OTA 파티션에 기록하기 위함 (ota.ino)

#define PN532_SCK   4
#define PN532_MISO  6
#define PN532_MOSI  5
#define PN532_SS1   7

// GetGeneralStatus(0x04) 응답의 Err 바이트(COLL/PARITY 등)를 직접 읽기 위해
// protected readdata()만 노출시킨 서브클래스 (공용 Adafruit_PN532 라이브러리는 수정하지 않음)
class Adafruit_PN532_Debug : public Adafruit_PN532 {
public:
  using Adafruit_PN532::Adafruit_PN532;
  using Adafruit_PN532::readdata;
};

#define HWSERIAL_RX 2
#define HWSERIAL_TX 9

#endif
