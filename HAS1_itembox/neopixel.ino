#include "neopixel_hal.h"
#include "library_and_pin.h"

#define LED_BRIGHTNESS 127
static const int NeopixelNum  = 2;
static const int NumPixels[2] = {28, 24};

Adafruit_NeoPixel pixels[2] = {
    Adafruit_NeoPixel(28, PN532_NEOPIXEL_PIN,   NEO_GRB + NEO_KHZ800),
    Adafruit_NeoPixel(24, ENCODER_NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800)
};

// NeoColor 인덱스와 순서가 일치해야 한다 (BLACK=0 ... PURPLE=6)
static int colorTable[7][3] = {
    {0,   0,   0  },  // BLACK
    {255, 255, 255},  // WHITE
    {255, 0,   0  },  // RED
    {255, 255, 0  },  // YELLOW
    {0,   255, 0  },  // GREEN
    {0,   0,   255},  // BLUE
    {255, 0,   255},  // PURPLE
};
// 엔코더 거리 단계별 파란색 농도 (내부 전용)
static int encBlue[4][3] = {{0,0,64},{0,0,128},{0,0,192},{0,0,255}};

static void setColor(int stripIdx, int c[3]) {
    pixels[stripIdx].fill(pixels[stripIdx].Color(c[0], c[1], c[2]));
    pixels[stripIdx].show();
}

void NeopixelInit() {
    for (int i = 0; i < NeopixelNum; ++i) {
        pixels[i].begin();
        pixels[i].setBrightness(LED_BRIGHTNESS);
    }
    NeoSetAll(WHITE);
}

void NeoSetAll(NeoColor c) {
    for (int i = 0; i < NeopixelNum; ++i)
        setColor(i, colorTable[c]);
}

void NeoSet(NeoStrip strip, NeoColor c) {
    if (strip >= NeopixelNum) return;  // NEO_INNER 등 미연결 스트립 무시
    setColor(strip, colorTable[c]);
}

void NeoSetBrightness(int b) {
    b = constrain(b, 0, 255);
    for (int i = 0; i < NeopixelNum; ++i) {
        pixels[i].setBrightness(b);
        pixels[i].show();
    }
}

void NeoEncoderUpdate() {
    long val   = readEncoderValue();
    int  grade = (int)(val / 24);        // 0~3: 파란색 농도 단계
    int  pos   = 23 - (int)(val % 24);  // 빨간 마커 위치
    for (int i = 0; i < NumPixels[NEO_ENCODER]; i++)
        pixels[NEO_ENCODER].setPixelColor(i, pixels[NEO_ENCODER].Color(encBlue[grade][0], encBlue[grade][1], encBlue[grade][2]));
    pixels[NEO_ENCODER].setPixelColor(pos, pixels[NEO_ENCODER].Color(colorTable[RED][0], colorTable[RED][1], colorTable[RED][2]));
    pixels[NEO_ENCODER].show();
}

// 50ms Runnable 훅 — ANIM 상태일 때 상태 함수(CorrectAnimState 등)가 blinkR.due를 직접 읽어 처리.
// neopixel HAL 단독 LED 유지 작업이 생기면 여기에 추가.
void BlinkHalUpdate() {}
