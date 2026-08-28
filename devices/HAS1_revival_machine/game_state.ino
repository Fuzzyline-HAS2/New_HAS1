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

    // JsonDocument(크기 템플릿 없는 v7 타입) 사용 — StaticJsonDocument<N>은 N이 my와 정확히
    // 같아야만 대입(operator=)이 되는데, 로컬/CI에 깔린 HAS2_Wifi 사본마다 my의 선언 크기가
    // 다를 수 있어(예: 1000 vs 2048) 매번 컴파일 에러가 났다(HAS1_itembox와 동일 이슈).
    // JsonDocument는 크기에 상관없이 대입/set()이 되므로 어떤 환경에서도 안전하다.
    static JsonDocument cur;

    bool brightness_changed = ((int)my["brightness"] != (int)cur["brightness"]);

    if ((String)(const char *)my["game_state"] != (String)(const char *)cur["game_state"])
    {
        if ((String)(const char *)my["game_state"] == "setting")
        {
            if (brightness_changed) SetBrightness((int)my["brightness"]);
            SettingFunc();
        }
        else if ((String)(const char *)my["game_state"] == "ready")
        {
            if (brightness_changed) SetBrightness((int)my["brightness"]);
            ReadyFunc();
        }
        else if ((String)(const char *)my["game_state"] == "activate")
        {
            if (brightness_changed) SetBrightness((int)my["brightness"]);
            ActivateRunOnce();
        }
    }
    else if (brightness_changed)
    {
        SetBrightness((int)my["brightness"]);
    }

    if ((String)(const char *)my["device_state"] != (String)(const char *)cur["device_state"])
    {
        if ((String)(const char *)my["device_state"] == "activate")
        {
            NeopixelSet(yellow);   // activate - 네오픽셀 전체 노란색(고정)
            SolenoidOff();         // 재무장 신호일 뿐 태그 이벤트가 아니므로 통전하지 않음
            NeoFunc = NeoNo;       // 호흡 애니메이션 없음
            // 태그로 문이 열릴 수 있는 구간이므로 폴링을 300ms로 좁혀
            // device_state="open" 반영 지연을 줄인다.
            SetWifiPollInterval(WIFI_POLL_INTERVAL_ACTIVATE_MS);
        }
        else if ((String)(const char *)my["device_state"] == "open")
        {
            NeopixelSet(blue);   // 서버가 태그를 승인 - 네오픽셀 전체 파란색(고정)
            SolenoidPulse(SOLENOID_REVIVAL_PULSE_MS);  // 승인 확정 시점에 실제로 문을 연다
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
        else if ((String)(const char *)my["device_state"] == "tagger")
        {
            NeopixelSet(purple);   // tagger - 네오픽셀 전체 보라색(고정)
            SolenoidOff();         // tagger 상태에서는 열리면 안 되므로 통전하지 않는다.
            NeoFunc = NeoNo;
            SetWifiPollInterval(WIFI_POLL_INTERVAL_DEFAULT_MS);  // 사용 불가 상태라 급하게 폴링할 필요 없음
        }
        else if ((String)(const char *)my["device_state"] == "github")
        {
            ota.check();
        }
    }

    Serial.println("Data Change");
    cur = my;
}
