#ifndef _LIBRARY_AND_PIN
#define _LIBRARY_AND_PIN

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <SecureOTA.h>
#include <SimpleTimer.h>
#include <SoftwareSerial.h>
#include "DFRobotDFPlayerMini.h"
#include <ESP32Encoder.h>
#include <Adafruit_PN532.h>
#include <WiFi.h>
#include <HAS2_Wifi.h>  // has2wifi 객체, my/shift_machine JSON 전역 (wifi.ino 전용)
#include "secrets.h"    // HMAC_SECRET (깃에 올라가지 않음 — .gitignore 처리)

// WiFi 접속 정보 — OTA 빌드에 포함되도록 코드에 직접 기입 (secrets.h 아님)
// 임시 고정: badland 테마 서버(172.30.1.43)가 아직 응답하지 않아 badland_ruins로 롤백.
// TODO: badland 테마 서버 문제 해결되면 wifi.ino의 Setup("badland")로 되돌릴 것.
#define WIFI_SSID     "badland_ruins"
#define WIFI_PASSWORD "Code3824@"

// DFPLAYER
#define DFPLAYER_RX_PIN 39
#define DFPLAYER_TX_PIN 33

// PN532
#define PN532_SCK 18
#define PN532_MISO 19
#define PN532_MOSI 23
#define PN532_SS1 5

// NEOPIXEL
#define PN532_NEOPIXEL_PIN 25
#define ENCODER_NEOPIXEL_PIN 27

// LINER MOTOR (BTS7960 모터 드라이버)
// R_EN, L_EN은 5V 직결
#define LINER_RPWM_PIN 32 
#define LINER_LPWM_PIN 4

// MICRO SWITCH
#define LINER_MOTOR_STOP_SWITCH 36

// 진동 모터 (TB6612FNG 모터 드라이버)
// AIN1->3.3V, AIN2->GND, STBY->3.3V
#define MOTOR_PWMA_PIN  14

// ENCODER
#define ENCODER_A_PIN 13
#define ENCODER_B_PIN 15
#define ENCODER_BUTTON_PIN 34 // 퍼즐을 풀 때 퍼즐 정답을 누르는 용도의 스위치
#endif
