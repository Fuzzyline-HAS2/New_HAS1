#ifndef _LIBRARY_AND_PIN_H_
#define _LIBRARY_AND_PIN_H_

// 라이브러리 선언
#include <Wire.h>
#include <SPI.h>
#include <Esp.h>
#include <Arduino.h>

#include <HAS2_Wifi.h>

#include <Adafruit_NeoPixel.h>
#include <Nextion.h>
#include <Adafruit_PN532.h>
#include <DFRobotDFPlayerMini.h>

#include <esp_task_wdt.h>
#include <SimpleTimer.h>

#include <SecureOTA.h>
#include "secrets.h"

// #define SERIAL1_RX_PIN 36    // 사용X
// #define SERIAL1_TX_PIN 32    // 사용X   

#define SERIAL2_RX_PIN 39   // MP3 Player RX
#define SERIAL2_TX_PIN 33   // MP3 Player TX

#define NEO_LINE    25  // 덕트 태그머신에 긴 네오픽셀
#define NEO_ROUND   26  // 덕트 태그머신에 pn532 주변 네오픽셀
#define NEO_SWITCH  27  // 덕트 스위치에 있는 네오픽셀


#define PN532_SCK   18     // PN532
#define PN532_MISO  19     // PN532
#define PN532_MOSI  23     // PN532
#define PN532_SS    5      // PN532

#define EMNERGENCY_CHK_PIN   14  // 덕트 스위치에 있는 비상탈출 눌렸을때 인식용
#define SW_PIN      15          // 덕트 스위치에 있는 탈출용 스위치
#define RELAY_PIN    21          // EM락 ON/OFF용 릴레이
#define EMCHECK_PIN  22          // EM락 닫혔는지 확인용 핀
#endif