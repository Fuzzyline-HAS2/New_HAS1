#include "HAS1_revival_machine.h"

/**
 * @brief DB gamestate가 setting 일 때 한번동작하는 코드
 */
void SettingFunc()
{
    activate_bool = true;  // setting 상태에서도 카드 태그를 감지해야 함(RfidLoop 활성화)
    NeoFunc = NeoNo;
    NeopixelSet(white);
    SolenoidOff();
    SetWifiPollInterval(WIFI_POLL_INTERVAL_DEFAULT_MS);
}

/**
 * @brief DB gamestate가 ready 일 때 한번동작하는 코드
 */

void ReadyFunc()
{
    activate_bool = false;  // ready 상태에서는 태그 감지도 하지 않음
    NeopixelSet(red);   // ready - 네오픽셀 전체 빨간색(고정)
    SolenoidOff();      // ready 상태는 통전하지 않음
    NeoFunc = NeoNo;    // 호흡 애니메이션 없음
    SetWifiPollInterval(WIFI_POLL_INTERVAL_DEFAULT_MS);
}

/**
 * @brief DB gamestate가 activate 일 때 반복동작하는 코드
 */
void ActivateFunc()
{
    RfidLoop();
}

/**
 * @brief DB gamestate가 activate 일 때 한번동작하는 코드
 */
void ActivateRunOnce()
{
    activate_bool = true;
    // 폴링 주기는 game_state가 아니라 device_state == "activate" 여부로 결정한다
    // (DataChange() 하단 device_state 분기 참고).
}

void DataChange()
{
    const char *device_name = (const char *)my["device_name"];
    BleAdvertiserUpdateFromDeviceName(device_name);

    if (!device_name)
    {
        Serial.println("[DataChange] 서버 데이터 없음, 스킵");
        return;
    }

    static JsonDocument cur;
    if (CardUploadSyncMode((const char *)my["device_state"]))
    {
        if (revival_approval_pending) EndRevivalApproval("card upload");
        last_open_tag_user = "";
        ghost_open_pending = false;
        gameplay_tag_latched = false;
        gameplay_tag_user = "";
        gameplay_tag_missing = false;
        gameplay_tag_miss_count = 0;
        activate_bool = false;
        SolenoidOff();
        NeoFunc = NeoNo;
        SetBrightness((int)my["brightness"]);
        NeopixelSet(purple);
        SetWifiPollInterval(WIFI_POLL_INTERVAL_DEFAULT_MS);
        cur = my;
        return;
    }

    UpdateRevivalApprovalState();
    // 시간초과/HTTP 실패 뒤 보존한 사용자도 게임 종료나 기기 취소 상태에서 정리한다.
    // open은 늦은 승인일 수 있으므로 아래의 기존 is_open 기록까지 보존한다.
    if (last_open_tag_user.length() &&
        ((String)(const char *)my["game_state"] != "activate" ||
         ((String)(const char *)my["device_state"] != "open" &&
          (String)(const char *)my["device_state"] != revival_request_device_state)))
    {
        last_open_tag_user = "";
    }
    if (ghost_open_pending) ++ghost_poll_count;

    // JsonDocument(크기 템플릿 없는 v7 타입) 사용 — StaticJsonDocument<N>은 N이 my와 정확히
    // 같아야만 대입(operator=)이 되는데, 로컬/CI에 깔린 HAS2_Wifi 사본마다 my의 선언 크기가
    // 다를 수 있어(예: 1000 vs 2048) 매번 컴파일 에러가 났다(HAS1_itembox와 동일 이슈).
    // JsonDocument는 크기에 상관없이 대입/set()이 되므로 어떤 환경에서도 안전하다.

    // 밝기 먼저 반영 — 이어지는 상태 전환이 새 밝기로 칠해지도록. 변경 감지는 SetBrightness() 내부.
    SetBrightness((int)my["brightness"]);

    const String game_state_now   = (String)(const char *)my["game_state"];
    const String device_state_now = (String)(const char *)my["device_state"];
    const bool game_changed   = game_state_now   != (String)(const char *)cur["game_state"];
    const bool device_changed = device_state_now != (String)(const char *)cur["device_state"];
    // 기기의 모드(색, 폴링 주기, 태그 활성)는 game_state와 device_state 두 값의 조합으로 정해진다.
    // 둘 중 하나만 바뀌어도 조합이 바뀌므로, 어느 쪽이 바뀌든 모드를 다시 계산한다.
    // 예전처럼 각 필드의 전이에만 반응하면 ready -> activate 로 갈 때 device_state가 이미
    // "activate"였던 경우 노란색/activate 폴링이 적용되지 않고 ready의 빨간색에 머물렀다.
    const bool state_changed = game_changed || device_changed;

    if (game_changed && game_state_now != "activate")
    {
        gameplay_tag_latched = false;
        gameplay_tag_user = "";
        gameplay_tag_missing = false;
        gameplay_tag_miss_count = 0;
    }

    if (state_changed)
    {
        if (game_state_now == "setting")
        {
            SettingFunc();
        }
        else if (game_state_now == "ready")
        {
            ReadyFunc();
        }
        else if (game_state_now == "activate")
        {
            ActivateRunOnce();
        }
    }

    // activate 게임 중의 기기 모드. 색/폴링/솔레노이드 OFF만 다루는 멱등 동작이라 다시 계산해도
    // 안전하다. game_state가 ready/setting이면 그쪽 색(빨강/흰색)이 우선이므로 여기서 덧칠하지 않는다.
    if (state_changed && game_state_now == "activate")
    {
        if (device_state_now == "activate")
        {
            NeopixelSet(yellow);   // activate - 네오픽셀 전체 노란색(고정)
            SolenoidOff();         // 재무장 신호일 뿐 태그 이벤트가 아니므로 통전하지 않음
            NeoFunc = NeoNo;       // 호흡 애니메이션 없음
            SetWifiPollInterval(WIFI_POLL_INTERVAL_ACTIVATE_MS);
        }
        else if (device_state_now == "tagger")
        {
            NeopixelSet(purple);   // tagger - 네오픽셀 전체 보라색(고정)
            SolenoidOff();         // tagger 상태에서는 열리면 안 되므로 통전하지 않는다.
            NeoFunc = NeoNo;
            SetWifiPollInterval(WIFI_POLL_INTERVAL_DEFAULT_MS);  // 사용 불가 상태라 급하게 폴링할 필요 없음
        }
    }

    // 한 번만 일어나야 하는 동작(솔레노이드 통전, is_open 기록, OTA 확인)은 device_state의
    // 전이에만 묶는다. game_state가 바뀌었다고 문을 다시 열거나 OTA를 다시 확인하면 안 된다.
    if (device_changed)
    {
        if (device_state_now == "open" && !CardUploadBlocksOpen())
        {
            NeopixelSet(blue);   // 서버가 태그를 승인 - 네오픽셀 전체 파란색(고정)
            int rssiOpen = WiFi.RSSI();
            uint32_t freeHeap = ESP.getFreeHeap();
            // 승인 후에는 콘솔 출력보다 먼저 연다. HIGH 직후 시각을 반환하므로
            // 로그를 5초 펄스 이후에 처리해도 total_ms에는 통전 시간이 섞이지 않는다.
            unsigned long relayOnMs = SolenoidPulse(SOLENOID_REVIVAL_PULSE_MS);
            if (ghost_open_pending)
            {
                unsigned long totalMs = relayOnMs - ghost_tag_start_ms;
                ghost_open_pending = false;
                Serial.println("[GhostTiming] RELAY ON: " + String(totalMs) + "ms polls=" + String(ghost_poll_count) +
                               " role_receive=" + String(ghost_role_receive_ms) + "ms situation=" + String(ghost_situation_ms) + "ms rssi_tag=" + String(ghost_rssi_at_tag) +
                               " rssi_open=" + String(rssiOpen) + " heap=" + String(freeHeap));
            }

            NeoFunc = NeoNo;
            SetWifiPollInterval(WIFI_POLL_INTERVAL_DEFAULT_MS);  // 이미 확정됐으니 폴링 다시 완화

            // 생존자가 생명장치를 열었으므로, 태그했던 iotGlove의 is_open을 true로 기록한다
            // (is_open은 생명장치가 아니라 iotGlove 쪽 필드).
            if (last_open_tag_user.length())
            {
                Serial.println("[GameState] device_state=open confirmed - marking is_open=1 for iotGlove: " + last_open_tag_user);
                has2wifi.Send(last_open_tag_user, "is_open", "1");
                Serial.println("[GameState] is_open=1 write request sent for: " + last_open_tag_user);
                last_open_tag_user = "";
            }
        }
        else if (device_state_now == "github" && !CardUploadBlocksGameplay())
        {
            ota.check();
        }
    }

    Serial.println("Data Change");
    cur = my;
}
