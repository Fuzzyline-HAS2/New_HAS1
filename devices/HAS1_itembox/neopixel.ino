#include "hal.h"
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
// 엔코더 링 배경색 (내부 전용) — 예전엔 24칸(한 바퀴)마다 4단계로 밝기가 계단식으로
// 바뀌어서 돌리다 보면 갑자기 밝아졌다/어두워졌다 하는 것처럼 보였다. 단일 농도로 통일.
static int encBlue[3] = {0, 0, 128};

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

void NeoEncoderUpdate(long value) {
    int  pos = 23 - (int)(value % 24);  // 빨간 마커 위치 (24칸 링을 한 바퀴씩 돎)
    for (int i = 0; i < NumPixels[NEO_ENCODER]; i++)
        pixels[NEO_ENCODER].setPixelColor(i, pixels[NEO_ENCODER].Color(encBlue[0], encBlue[1], encBlue[2]));
    pixels[NEO_ENCODER].setPixelColor(pos, pixels[NEO_ENCODER].Color(colorTable[RED][0], colorTable[RED][1], colorTable[RED][2]));
    pixels[NEO_ENCODER].show();
}

// 50ms Runnable 훅 — ANIM 상태일 때 상태 함수(CorrectAnimState 등)가 blinkR.due를 직접 읽어 처리.
// neopixel HAL 단독 LED 유지 작업이 생기면 여기에 추가.
void BlinkHalUpdate() {}
