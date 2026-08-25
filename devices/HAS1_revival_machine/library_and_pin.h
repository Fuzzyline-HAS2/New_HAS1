#ifndef _LIBRARY_AND_PIN_H_
#define _LIBRARY_AND_PIN_H_
// 라이브러리 선언

#include <Wire.h>
#include <SPI.h>
#include <Esp.h>
#include <Arduino.h>

#include <HAS2_Wifi.h>

#include <Adafruit_NeoPixel.h>
#include <Adafruit_PN532.h>

#include <SimpleTimer.h>
#include <esp_bt.h>

#include <SecureOTA.h>
#include "secrets.h"


// 핀 선언

// #define SERIAL1_RX_PIN 36 // 미사용
// #define SERIAL1_TX_PIN 32

#define NEOPIXEL_TOP_PIN 25
#define NEOPIXEL_MID_PIN 26
#define NEOPIXEL_BOT_PIN 27

#define SOLENOID_PIN 14  // 솔레노이드 데이터 핀 (모스펫 구동)
#define SOLENOID_PULSE_MS 2000  // 잠금/해제 순간에만 통전시키는 기본 펄스 길이(ms). 래치 없는 솔레노이드라 계속 통전시키면 발열/소손 위험 — 기구가 동작하는 최소 시간으로 실측 후 조정할 것. (500ms는 딸깍 소리만 나고 실제로 안 열려서 2000ms로 연장)
#define SOLENOID_REVIVAL_PULSE_MS 5000  // revival 태그로 열릴 때는 넉넉하게 5초 통전

// wifi_timer(서버 폴링) 주기. 평소엔 2초로 서버 부하를 아끼고, activate 상태(태그로
// 문이 열릴 수 있는 구간)에서만 300ms로 좁혀 device_state="open" 반영 지연을 줄인다.
#define WIFI_POLL_INTERVAL_DEFAULT_MS 2000
#define WIFI_POLL_INTERVAL_ACTIVATE_MS 300

#define PN532_SCK                       (18)
#define PN532_MISO                      (19)
#define PN532_MOSI                      (23)
#define PN532_SS                        (5) 
#endif
