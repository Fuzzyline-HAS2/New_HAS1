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

#define HWSERIAL_RX 2
#define HWSERIAL_TX 9

#endif
