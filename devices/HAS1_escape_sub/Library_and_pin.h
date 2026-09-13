#ifndef _LIBRARY_AND_PIN_
#define _LIBRARY_AND_PIN_

#include <Arduino.h>

#include <SPI.h>
#include <Adafruit_PN532.h>
#include <HardwareSerial.h>
#include <SimpleTimer.h>

#define PN532_SCK   4
#define PN532_MISO  5
#define PN532_MOSI  6
#define PN532_SS1   7
#define PN532_SS2   9
#define PN532_SS3   2

// 미사용. TTGO와의 통신은 Serial(=UART0)이 담당하며, ESP32-C3의 UART0 기본 핀은
// TX=GPIO21 / RX=GPIO20 이다. 아래 HWSERIAL_RX(8)은 실제 배선과 무관한 잔재이며
// 코드 어디에서도 참조하지 않는다(fromSubSerial/toMainSerial도 begin() 호출 없는 죽은 선언).
#define HWSERIAL_RX 8
#define HWSERIAL_TX 21

#endif
