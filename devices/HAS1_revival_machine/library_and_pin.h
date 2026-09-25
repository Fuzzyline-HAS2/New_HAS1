#ifndef _LIBRARY_AND_PIN_H_
#define _LIBRARY_AND_PIN_H_
// 라이브러리 선언

#include <Wire.h>
#include <SPI.h>
#include <Esp.h>
#include <Arduino.h>

#include <HAS2_Wifi.h>

#include <Adafruit_NeoPixel.h>
#include <Adafruit_SPIDevice.h>

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

// wifi_timer(서버 폴링) 주기.
#define WIFI_POLL_INTERVAL_DEFAULT_MS 2000
// activate 구간 폴링 주기. 서버가 보내는 상태 변경(tagger 봉쇄, 재무장 등)을 인지하는 용도.
// 개방 승인 자체는 v49부터 승인 대기 중 PollRevivalApproval()이 300ms로 ReceiveMine()을 직접
// 조회해 잡으므로(approval.ino) 이 값은 개방 지연에는 관여하지 않는다.
//
// 실험 이력:
//  - 300 -> 700 (v39): 잘못된 측정으로 되돌림. 마커로 쓴 "[GameState] device_state=open
//    confirmed" 로그가 SolenoidPulse(5000) 뒤에 찍혀 5초 늦게 관측된 것이었다.
//  - 300 -> 2000 (v51): "글러브를 PN532에 밀착 유지하면 activate에서만 판독이 늦고 open에서는
//    거리와 무관하다"의 원인이 activate/open 간 유일한 코드 차이인 이 폴링 주기인지 실험.
//    현장 결과 밀착 판독은 그대로였다 -> 기각. tagger 봉쇄 인지가 최대 2초 늦어지는 비용만
//    남으므로 300으로 되돌린다. 이 실험만으로 RF/전송 계층 원인을 확정할 수는 없다.
#define WIFI_POLL_INTERVAL_ACTIVATE_MS 300

#define PN532_SCK                       (18)
#define PN532_MISO                      (19)
#define PN532_MOSI                      (23)
#define PN532_SS                        (5) 
#endif
