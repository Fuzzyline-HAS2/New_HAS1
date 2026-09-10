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
// activate 구간 폴링 주기. 300ms였을 때 오히려 반영이 6배 느렸다 (현장 계측 2026-09-10):
//   폴링 2000ms 구간 → 서버 변경 인지 867 / 909 ms
//   폴링  300ms 구간 → 서버 변경 인지 5426 / 5587 ms
// 원인: 같은 로그에서 측정된 HTTP 왕복이 237~336ms(평균 282ms)로 300ms 주기와 거의 같아,
// WifiTimerFunc(has2wifi.Loop)가 끝나기 전에 다음 주기가 도래해 폴링이 연속 실행된다.
// loop()가 HTTP 대기에 묶이고, 실패·지연이 겹친 사이클의 shift_machine 플래그를 놓쳐
// (HAS2_Wifi::Loop는 그 플래그가 선 사이클에만 행을 읽는다) 다음 기회까지 밀린다.
// 왕복시간보다 확실히 큰 값으로 두어 매 사이클이 여유롭게 완료되게 한다.
#define WIFI_POLL_INTERVAL_ACTIVATE_MS 700

#define PN532_SCK                       (18)
#define PN532_MISO                      (19)
#define PN532_MOSI                      (23)
#define PN532_SS                        (5) 
#endif
