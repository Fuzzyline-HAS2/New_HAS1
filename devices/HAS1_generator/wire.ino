// =================================================================================
// wire.ino
// ---------------------------------------------------------------------------------
// 배선 4개(WIRE_PIN_1~4) 감지 — 평소 INPUT_PULLUP 1(floating), 배선(GND) 꽂히면 0.
// 배터리팩 충전을 RFID 태그(기존 BatteryPackCharge) 대신 물리 배선 개수로 실시간 반영한다.
// 뽑으면 즉시 감소 — 태그 방식과 달리 되돌릴 수 있는 상태값.
//
// WirePollMain()은 ActivateFunc()에서 배터리팩 충전 단계에 들어갈 때 ptrCurrentMode로 등록되어
// loop()마다 호출되며, 배선 개수가 바뀌면 디바운스를 거쳐 my["battery_pack"]과 서버·LED에 반영한다.
// =================================================================================

static const uint8_t WIRE_PINS[4] = { WIRE_PIN_1, WIRE_PIN_2, WIRE_PIN_3, WIRE_PIN_4 };
// 기계식 접점 튐(bounce) 방지 — 커넥터를 꽂는 순간 짧게 튀는 접점을 30ms로는 다 걸러내지
// 못해 "꽂았는데 순간적으로 빠진 걸로 확정"되는 오판이 있어 100ms로 늘림
static const unsigned long WIRE_DEBOUNCE_MS = 100;

static unsigned long wireLastSampleTime = 0; // 마지막으로 핀 상태를 샘플링한 시각 (20ms 주기 샘플링용)
static int            wireCandidateCnt   = -1; // 아직 확정되지 않은, 방금 새로 읽힌 배선 개수 후보
static unsigned long  wireCandidateSince = 0;  // wireCandidateCnt가 그 값으로 유지되기 시작한 시각
static int            wireStableCnt      = -1; // 디바운스를 통과해 실제로 확정(반영)된 배선 개수

// setup()에서 1회 호출: 배선 감지 핀 4개를 내부 풀업(INPUT_PULLUP)으로 설정한다.
void WireInit() {
    for (int i = 0; i < 4; i++) pinMode(WIRE_PINS[i], INPUT_PULLUP);
}

// 현재 이 순간 꽂혀 있는(LOW로 읽히는) 배선 개수를 즉시 읽어 반환한다 (디바운스 없음).
int WireCountPlugged() {
    int cnt = 0;
    for (int i = 0; i < 4; i++)
        if (digitalRead(WIRE_PINS[i]) == LOW) cnt++;
    return cnt;
}

// ActivateFunc()이 배선 충전 단계로 들어갈 때마다 호출 — 이전 기억을 버리고
// 다음 WirePollMain() 호출에서 실물 배선 상태로 강제 재동기화되게 함
void WireResetTracking() {
    wireCandidateCnt = -1;
    wireStableCnt = -1;
    batteryFinishDone = false; // 새 충전 사이클 시작 — BatteryFinish()가 다시 한 번 실행되도록 재무장
}

// ptrCurrentMode로 등록되어 loop()마다 호출됨 (기존 RfidLoopMain 자리)
// 동작 순서:
//   1) 20ms보다 자주 재샘플링하지 않음 (과도한 폴링 방지)
//   2) 읽은 개수가 후보(wireCandidateCnt)와 다르면, 새 후보로 교체하고 타이머를 리셋한 뒤 리턴
//      (=값이 흔들리는 동안에는 확정하지 않음, 기계식 스위치 채터링 방지)
//   3) 후보가 이미 확정값(wireStableCnt)과 같다면 할 일 없음
//   4) 후보가 WIRE_DEBOUNCE_MS(100ms) 이상 안정적으로 유지됐다면 그제서야 확정 처리:
//      변화량(delta)을 계산해 my["battery_pack"]을 갱신하고, 서버에 증감치를 보내고,
//      게이지 LED(BatteryPackSend)를 갱신한다.
//   5) 확정된 개수가 최대치(max_battery_pack)에 도달하면 BatteryFinish()로 다음 단계 진행.
void WirePollMain() {
    if (millis() - wireLastSampleTime < 20) return;
    wireLastSampleTime = millis();

    int wireCnt = WireCountPlugged();
    if (wireCnt != wireCandidateCnt) {
        wireCandidateCnt = wireCnt;
        wireCandidateSince = millis();
        return;
    }
    if (wireCandidateCnt == wireStableCnt) return;
    if (millis() - wireCandidateSince < WIRE_DEBOUNCE_MS) return;

    // wireStableCnt가 아직 미확정(-1)이면 delta=0으로 취급(최초 확정은 증감 알림 없이 절대값만 반영)
    int delta = wireCandidateCnt - (wireStableCnt < 0 ? wireCandidateCnt : wireStableCnt);
    wireStableCnt = wireCandidateCnt;
    my["battery_pack"] = wireStableCnt;
    SyncBatteryPackCur(); // cur도 같이 맞춰서 다음 서버 폴링이 이 변화를 또 새 변화로 착각하지 않게 함
    has2wifi.Send((String)(const char*)my["device_name"], "battery_pack", (delta >= 0 ? "+" : "") + String(delta));
    BatteryPackSend();
    if (delta > 0) Mp3PlayLargeFolder(1, 7);  // 배선이 꽂혀 게이지가 늘어날 때만 재생 (빠질 때는 재생 안 함)

    if (wireStableCnt >= (int)my["max_battery_pack"]) {
        BatteryFinish();
    }
}
