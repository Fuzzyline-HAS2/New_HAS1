// =================================================================================
// GaugeEncoderDemo.ino
// ---------------------------------------------------------------------------------
// HAS1_generator 실기(핀 번호 동일)에서 와이파이/RFID/DFPlayer/배선 감지 등은 전부 빼고
// "엔코더(손잡이)를 돌리면 GAUGE 네오픽셀이 파란색으로 차오른다"만 확인하기 위한 간단 예제.
//
// - 엔코더 카운팅은 실제 HAS1_generator(encoder.ino)와 동일하게 ESP32 하드웨어 펄스
//   카운터(PCNT)를 사용한다. A상/B상 모두 "엣지마다 +1"로 세므로 방향은 구분하지 않는다.
// - GAUGE 스트립(핀/픽셀 수 동일)에 encoderValue를 STARTER_ENCODER_UNIT으로 나눈
//   칸 수만큼 파란색을 채운다.
// - 시리얼 모니터(115200bps)에 200ms마다 현재 encoderValue와 칸 수를 출력한다.
// =================================================================================
#include <Adafruit_NeoPixel.h>
#include "driver/pulse_cnt.h"

// ---- 핀 번호 (library_and_pin.h와 동일) ----
#define GAUGE_NEOPIXEL_PIN 25  // GAUGE 스트립 데이터 핀 (실기의 PN532_NEOPIXEL_PIN)
#define ENCODER_PIN_A 13
#define ENCODER_PIN_B 15

// ---- GAUGE 스트립 설정 (HAS1_generator.h와 동일) ----
#define GAUGE_PIXEL_COUNT 28
#define DEFAULT_BRIGHTNESS 50
Adafruit_NeoPixel gauge(GAUGE_PIXEL_COUNT, GAUGE_NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// 게이지 1칸을 채우는 데 필요한 엔코더 값 (HAS1_generator.h의 starterEncoderUnit 기본값과 동일)
int starterEncoderUnit = 4000;
long encoderValue = 0;

// ---- 엔코더 (ESP32 하드웨어 PCNT — encoder.ino와 동일 방식) ----
pcnt_unit_handle_t pcntUnit = NULL;
int lastPcntCount = 0;

void EncoderInit() {
    pinMode(ENCODER_PIN_A, INPUT_PULLUP);
    pinMode(ENCODER_PIN_B, INPUT_PULLUP);

    pcnt_unit_config_t unitConfig = {};
    unitConfig.low_limit = -32768;
    unitConfig.high_limit = 32767;
    pcnt_new_unit(&unitConfig, &pcntUnit);

    pcnt_glitch_filter_config_t filterConfig = {};
    filterConfig.max_glitch_ns = 1000;
    pcnt_unit_set_glitch_filter(pcntUnit, &filterConfig);

    pcnt_chan_config_t chanConfig = {};
    chanConfig.edge_gpio_num = ENCODER_PIN_A;
    chanConfig.level_gpio_num = GPIO_NUM_NC;
    pcnt_channel_handle_t chanA = NULL;
    pcnt_new_channel(pcntUnit, &chanConfig, &chanA);
    pcnt_channel_set_edge_action(chanA, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(chanA, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP);

    chanConfig.edge_gpio_num = ENCODER_PIN_B;
    pcnt_channel_handle_t chanB = NULL;
    pcnt_new_channel(pcntUnit, &chanConfig, &chanB);
    pcnt_channel_set_edge_action(chanB, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(chanB, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP);

    pcnt_unit_enable(pcntUnit);
    pcnt_unit_clear_count(pcntUnit);
    pcnt_unit_start(pcntUnit);
}

// 매 loop마다 호출: 하드웨어 카운터의 증가분을 encoderValue에 누적한다.
void EncoderLoop() {
    int count = 0;
    pcnt_unit_get_count(pcntUnit, &count);
    int delta = count - lastPcntCount;
    lastPcntCount = count;
    if (delta < 0) delta += 32767; // 카운터가 high_limit을 넘어 랩어라운드한 경우 보정

    if (delta != 0) encoderValue += delta;

    // 하드웨어 한계(32767) 도달 전에 미리 리베이스
    if (count > 20000) {
        pcnt_unit_clear_count(pcntUnit);
        lastPcntCount = 0;
    }
}

// litCount번째 픽셀까지 파란색, 나머지는 꺼진 채로 GAUGE 스트립을 갱신한다.
void GaugeShow(int litCount) {
    for (int i = 0; i < GAUGE_PIXEL_COUNT; i++) {
        if (i < litCount) gauge.setPixelColor(i, gauge.Color(0, 0, 255)); // BLUE
        else               gauge.setPixelColor(i, 0);                    // OFF
    }
    gauge.show();
}

void setup() {
    Serial.begin(115200);

    gauge.begin();
    gauge.setBrightness(DEFAULT_BRIGHTNESS);
    GaugeShow(0);

    EncoderInit();

    Serial.println("GaugeEncoderDemo start — 손잡이를 돌려보세요.");
}

void loop() {
    EncoderLoop();

    int litCount = encoderValue / starterEncoderUnit;
    if (litCount < 0) litCount = 0;
    if (litCount > GAUGE_PIXEL_COUNT) litCount = GAUGE_PIXEL_COUNT;

    static int lastLitCount = -1;
    if (litCount != lastLitCount) {
        lastLitCount = litCount;
        GaugeShow(litCount);
    }

    static unsigned long lastPrint = 0;
    if (millis() - lastPrint >= 200) {
        lastPrint = millis();
        Serial.printf("encoderValue=%ld  gauge=%d/%d\n", encoderValue, litCount, GAUGE_PIXEL_COUNT);
    }
}
