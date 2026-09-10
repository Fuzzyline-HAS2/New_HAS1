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
// activate 구간 폴링 주기. 300 -> 700 실험(v39)은 실패했으므로 300으로 되돌림.
// 당시 근거였던 "폴링 300ms 구간이 2000ms 구간보다 6배 느리다"는 측정이 잘못이었다 —
// 마커로 쓴 "[GameState] device_state=open confirmed" 로그가 SolenoidPulse(5000)의
// delay 뒤에 찍혀서, 실제 개방 시점보다 5초 늦게 관측된 것이었다(game_state.ino 참고).
// 5000ms를 보정한 실측: 폴링 300ms -> 인지 426/587ms, 폴링 700ms -> 345~800ms.
// 이론치(주기/2 + HTTP왕복 282ms)와 일치하며 폴링은 원래부터 정상 동작했다. 700ms는 평균 39ms 악화.
#define WIFI_POLL_INTERVAL_ACTIVATE_MS 300

#define PN532_SCK                       (18)
#define PN532_MISO                      (19)
#define PN532_MOSI                      (23)
#define PN532_SS                        (5) 
#endif
