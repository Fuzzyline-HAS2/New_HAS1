#include "HAS1_revival_machine.h"

/**
 * @brief DB gamestate가 setting 일 때 한번동작하는 코드
 */
void SettingFunc()
{
    activate_bool = false;
    ghost_opened_local = false;  // 다음 라운드를 위해 로컬 잠금 해제
    NeoFunc = NeoNo;
    NeopixelSet(white);
    SolenoidOff();
}

/**
 * @brief DB gamestate가 ready 일 때 한번동작하는 코드
 */

void ReadyFunc()
{
    activate_bool = false;
    NeopixelSet(red);   // ready - 네오픽셀 전체 빨간색(고정)
    SolenoidPulse();
    NeoFunc = NeoNo;    // 호흡 애니메이션 없음
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
            ghost_opened_local = false;   // 재무장 → 로컬 잠금 해제(다음 ghost 태그 허용)
            NeopixelSet(yellow);   // activate - 네오픽셀 전체 노란색(고정)
            SolenoidPulse();
            NeoFunc = NeoNo;       // 호흡 애니메이션 없음
        }
        else if ((String)(const char *)my["device_state"] == "open")
        {
            NeopixelSet(blue);   // ghost 태그로 열림 - 네오픽셀 전체 파란색(고정)
            SolenoidOff();
            NeoFunc = NeoNo;
        }
        else if ((String)(const char *)my["device_state"] == "show_time")
        {
            // 이전 on/off 상태와 무관하게 강제로 펄스. 색상은 변경하지 않는다.
            SolenoidPulse();
        }
        else if ((String)(const char *)my["device_state"] == "github")
        {
            ota.check();
        }
    }

    Serial.println("Data Change");
    cur = my;
}
