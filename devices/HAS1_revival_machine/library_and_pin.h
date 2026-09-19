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

// wifi_timer(서버 폴링) 주기.
#define WIFI_POLL_INTERVAL_DEFAULT_MS 2000
// activate 구간 폴링 주기. 원래 300ms였고 목적은 device_state="open" 반영 지연 단축이었다.
// v49부터는 승인 대기 중 PollRevivalApproval()이 300ms 간격으로 ReceiveMine()을 직접 조회해
// 개방을 잡으므로(approval.ino), 유휴 activate 구간의 이 폴링은 개방 지연에 더 이상 관여하지
// 않는다. 남은 역할은 서버가 보내는 상태 변경(tagger 봉쇄, 재무장 등)을 인지하는 것뿐이다.
//
// 2000ms로 올린 이유(v51 실험): 현장에서 PN532에 글러브를 밀착 유지하면 activate에서만 판독이
// 늦거나 뗄 때 되고, open 상태에서는 거리와 무관하게 바로 읽힌다. 두 상태의 코드 차이는 이
// 폴링 주기 하나다. 300ms 주기는 HTTP 왕복(~280ms)과 거의 같아 activate 루프의 절반 이상이
// 블로킹 HTTP였고, PN532 판독 시도가 매번 Wi-Fi 송신 직후에 걸렸다(전원 스파이크/루프 점유).
// 밀착(<2cm)은 이 로트에서 마진이 없는 구간이라(sensor.ino RxGain 주석) 그 영향을 먼저 받는다.
//
// 비용: 서버가 device_state를 바꿔도(예: tagger 봉쇄) 기기가 최대 2초 늦게 인지한다.
// 실험 결과 밀착 판독이 개선되면 유지하고, 그대로면 gain 고정 실험으로 넘어간다.
//
// (참고) 300 -> 700 실험(v39)은 잘못된 측정으로 되돌렸었다: 마커로 쓴 "[GameState]
// device_state=open confirmed" 로그가 SolenoidPulse(5000) 뒤에 찍혀 5초 늦게 관측됐다.
#define WIFI_POLL_INTERVAL_ACTIVATE_MS 2000

#define PN532_SCK                       (18)
#define PN532_MISO                      (19)
#define PN532_MOSI                      (23)
#define PN532_SS                        (5) 
#endif
