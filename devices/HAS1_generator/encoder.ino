// 스타터 엔코더: HAS2 second_store와 같은 A/B CHANGE 인터럽트 상태 전이 판독.
// 정상 전이 8개는 방향과 관계없이 +1 (A/B 한 주기 = 4카운트).
// ISR은 대기 카운트만 쌓고, 게임 값/감소 타이머는 loop에서만 갱신한다.

static portMUX_TYPE encoderMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t encoderPendingCount = 0;
static volatile uint8_t encoderPreviousState = 0;
static volatile bool encoderAccepting = false;

void ARDUINO_ISR_ATTR EncoderInterrupt()
{
    portENTER_CRITICAL_ISR(&encoderMux);
    if (encoderAccepting) {
        uint8_t state = (digitalRead(encoderPinA) << 1) | digitalRead(encoderPinB);
        uint8_t transition = (encoderPreviousState << 2) | state;
        switch (transition) {
            case 0b1101: case 0b0100: case 0b0010: case 0b1011:
            case 0b1110: case 0b0111: case 0b0001: case 0b1000:
                ++encoderPendingCount;
                break;
        }
        // 동일 상태/두 비트 동시 변화는 세지 않되 현재 핀 상태로 재동기화한다.
        encoderPreviousState = state;
    }
    portEXIT_CRITICAL_ISR(&encoderMux);
}

// setup 1회: 게임에서 명시적으로 Attach할 때까지 카운팅하지 않는다.
void EncoderInit()
{
    pinMode(encoderPinA, INPUT_PULLUP);
    pinMode(encoderPinB, INPUT_PULLUP);
}

// 이미 동작 중이면 누적량/이전 상태를 유지한다. GPIO API는 임계구역 밖에서 호출한다.
void EncoderAttach()
{
    if (encoderAttached) return;
    attachInterrupt(digitalPinToInterrupt(encoderPinA), EncoderInterrupt, CHANGE);
    attachInterrupt(digitalPinToInterrupt(encoderPinB), EncoderInterrupt, CHANGE);
    portENTER_CRITICAL(&encoderMux);
    // 정지 중 움직인 양은 세지 않고 실제 시작 상태부터 판독한다.
    encoderPreviousState = (digitalRead(encoderPinA) << 1) | digitalRead(encoderPinB);
    encoderAccepting = true;
    portEXIT_CRITICAL(&encoderMux);
    encoderAttached = true;
}

void EncoderDetach()
{
    if (!encoderAttached) return;
    // 먼저 ISR 수집을 닫아 정지/잔여량 반영 사이에 새 카운트가 끼지 않게 한다.
    portENTER_CRITICAL(&encoderMux);
    encoderAccepting = false;
    portEXIT_CRITICAL(&encoderMux);
    detachInterrupt(digitalPinToInterrupt(encoderPinA));
    detachInterrupt(digitalPinToInterrupt(encoderPinB));
    encoderAttached = false;
    EncoderLoop(); // 이미 받아 둔 회전량은 정확히 한 번 반영한다.
}

// loop 문맥에서만 호출. ISR과 원자적으로 교환해 감소/라운드 리셋과 경쟁하지 않는다.
void EncoderLoop()
{
    portENTER_CRITICAL(&encoderMux);
    uint32_t delta = encoderPendingCount;
    encoderPendingCount = 0;
    portEXIT_CRITICAL(&encoderMux);
    if (delta != 0) {
        encoderValue += delta;
        gameTimerCnt = 0;
    }
}
