// =================================================================================
// encoder.ino
// ---------------------------------------------------------------------------------
// 스타터 손잡이(로터리 엔코더)의 회전량을 세는 파일.
// 엔코더 카운팅을 인터럽트(ISR) 대신 ESP32 하드웨어 펄스 카운터(PCNT)로 처리.
// CPU 개입 없이 하드웨어가 A/B 양핀의 모든 엣지를 세므로
// 네오픽셀 전송을 방해하지 않고, loop가 WiFi 등으로 막혀 있어도 펄스가 유실되지 않는다.
//
// 전체 흐름: EncoderInit()(setup 1회) → EncoderAttach()/EncoderDetach()(게임 진행에 맞춰 on/off)
// → EncoderLoop()(매 loop마다 호출, 하드웨어 카운트 증분을 전역 encoderValue에 누적).
// =================================================================================
#include "driver/pulse_cnt.h"

pcnt_unit_handle_t pcntUnit = NULL; // PCNT 하드웨어 유닛 핸들 (ESP-IDF pulse_cnt 드라이버 객체)
int lastPcntCount = 0;              // 직전 EncoderLoop() 호출 시점의 하드웨어 카운터 값 (증분 계산용)

// setup()에서 1회 호출: PCNT 유닛/글리치 필터/A·B 두 채널을 구성하고 카운팅을 시작한다.
// A상(encoderPinA)과 B상(encoderPinB) 양쪽 모두를 "상승 엣지마다 +1" 채널로 등록해두었기 때문에,
// 회전 방향 판별 없이 두 핀의 엣지를 합산한 총 펄스 수만 셈한다(단순 회전량 측정용, 방향 무시).
void EncoderInit()
{
    // 엔코더 A/B 두 핀 모두 내부 풀업 사용 (엔코더 쪽 출력이 오픈 드레인/스위치 방식이라고 가정)
    pinMode(encoderPinA, INPUT_PULLUP);
    pinMode(encoderPinB, INPUT_PULLUP);

    // 1) PCNT 유닛 생성 — 카운트 값이 오갈 수 있는 범위(하드웨어 레지스터 한계인 16비트 부호있는 범위)를 지정
    pcnt_unit_config_t unitConfig = {};
    unitConfig.low_limit = -32768;
    unitConfig.high_limit = 32767;
    esp_err_t err = pcnt_new_unit(&unitConfig, &pcntUnit);
    if (err != ESP_OK) {
        Serial.printf("[PCNT] unit create failed: %s\n", esp_err_to_name(err));
        return;
    }

    // 2) 글리치 필터 — 1000ns(1us)보다 짧게 튀는 신호(접점 노이즈)는 펄스로 인정하지 않음
    pcnt_glitch_filter_config_t filterConfig = {};
    filterConfig.max_glitch_ns = 1000;
    err = pcnt_unit_set_glitch_filter(pcntUnit, &filterConfig);
    if (err != ESP_OK) {
        Serial.printf("[PCNT] glitch filter failed: %s\n", esp_err_to_name(err));
        return;
    }

    // 3) A상 채널 등록 — edge_gpio_num만 지정하고 level_gpio_num은 사용 안 함(GPIO_NUM_NC)
    pcnt_chan_config_t chanConfig = {};
    chanConfig.edge_gpio_num = encoderPinA;
    chanConfig.level_gpio_num = GPIO_NUM_NC;
    pcnt_channel_handle_t chanA = NULL;
    esp_err_t ret = pcnt_new_channel(pcntUnit, &chanConfig, &chanA);
    if (ret != ESP_OK) {
        Serial.printf("[PCNT] chanA error: %s\n", esp_err_to_name(ret));
        return;
    }
    // 상승 엣지든 하강 엣지든 상관없이 무조건 카운트를 +1 증가시키도록 설정
    // (방향 판별을 하지 않는 단순 펄스 카운팅이므로 두 액션 모두 INCREASE로 동일하게 지정)
    pcnt_channel_set_edge_action(chanA, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(chanA, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP);

    // 4) B상 채널도 동일한 방식으로 등록 — 결과적으로 A/B 두 핀의 엣지가 하나의 카운터에 합산된다
    chanConfig.edge_gpio_num = encoderPinB;
    pcnt_channel_handle_t chanB = NULL;
    ret = pcnt_new_channel(pcntUnit, &chanConfig, &chanB);
    if (ret != ESP_OK) {
        Serial.printf("[PCNT] chanB error: %s\n", esp_err_to_name(ret));
        return;
    }
    pcnt_channel_set_edge_action(chanB, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(chanB, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP);

    // 5) 유닛 활성화 후 카운터를 0으로 초기화하고 카운팅 시작
    err = pcnt_unit_enable(pcntUnit);
    if (err != ESP_OK) {
        Serial.printf("[PCNT] unit enable failed: %s\n", esp_err_to_name(err));
        return;
    }
    pcnt_unit_clear_count(pcntUnit);
    pcnt_unit_start(pcntUnit);
    encoderAttached = true;
}

// 엔코더 카운팅을 (재)시작한다. 이미 카운팅 중이면 아무 것도 하지 않는다(중복 호출에 안전).
// 카운터를 0으로 리셋한 뒤 시작하므로, 매번 "0부터 새로 세는" 스타터 라운드에 적합하다.
void EncoderAttach()
{
    if (!encoderAttached){
        pcnt_unit_clear_count(pcntUnit);
        lastPcntCount = 0;
        pcnt_unit_start(pcntUnit);
        encoderAttached = true;
    }
}

// 엔코더 카운팅을 정지한다. 스타터 단계가 아닐 때 손잡이를 돌려도 encoderValue가
// 늘어나지 않도록 막는 용도(예: 태그가 없거나 다른 게임 상태일 때).
void EncoderDetach()
{
    if (encoderAttached){
        pcnt_unit_stop(pcntUnit);
        encoderAttached = false;
    }
}

// 매 loop마다 호출: 하드웨어 카운터의 증가분을 encoderValue에 반영.
// EncoderInit()에서 A/B 두 핀 모두 "엣지마다 +1"로 설정했기 때문에, 이 함수는 회전 방향을
// 계산하지 않고 오직 "지난 호출 이후 몇 틱이 늘었는가(delta)"만 계산해 누적한다.
void EncoderLoop()
{
    if (!encoderAttached) return; // 카운팅이 꺼져 있으면(EncoderDetach 상태) 아무 것도 하지 않음

    int count = 0;
    pcnt_unit_get_count(pcntUnit, &count); // 하드웨어 레지스터에서 현재 누적 카운트를 읽음
    int delta = count - lastPcntCount;     // 직전 호출 이후 늘어난 틱 수
    lastPcntCount = count;

    // loop가 오래 막힌 사이(WiFi 통신 등) 카운터가 high_limit(32767)를 넘으면
    // 하드웨어가 low_limit(0 근방)으로 랩어라운드해 delta가 음수가 됨 → 한 바퀴만큼 보정
    if (delta < 0) delta += 32767;

    if (delta != 0){
        encoderValue += delta;   // 스타터 게이지 계산에 쓰이는 누적값 (StarterActivate에서 참조)
        gameTimerCnt = 0; // 회전 감지 → 게이지 감소 타이머 리셋 (기존 ISR과 동일)
    }

    // 하드웨어 한계(32767) 도달 전에 카운터 리베이스: count가 임계값(20000)을 넘으면
    // 실제 랩어라운드가 일어나기 전에 미리 0으로 되돌려, 위의 delta<0 보정 경로를 최대한 피한다
    // (보정 경로는 "정확히 한 바퀴만큼"만 가정하므로, 자주 타지 않는 편이 더 안전하다).
    if (count > 20000){
        pcnt_unit_clear_count(pcntUnit);
        lastPcntCount = 0;
    }
}
