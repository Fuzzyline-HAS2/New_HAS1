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
            NeopixelSet(yellow);   // activate - 네오픽셀 전체 노란색(고정)
            SolenoidOff();         // 재무장 신호일 뿐 태그 이벤트가 아니므로 통전하지 않음
            NeoFunc = NeoNo;       // 호흡 애니메이션 없음
        }
        else if ((String)(const char *)my["device_state"] == "open")
        {
            NeopixelSet(blue);   // 서버가 태그를 승인 - 네오픽셀 전체 파란색(고정)
            SolenoidPulse(SOLENOID_REVIVAL_PULSE_MS);  // 승인 확정 시점에 실제로 문을 연다
            NeoFunc = NeoNo;
        }
        else if ((String)(const char *)my["device_state"] == "tagger")
        {
            NeopixelSet(purple);   // tagger - 네오픽셀 전체 보라색(고정)
            SolenoidOff();         // tagger 상태에서는 열리면 안 되므로 통전하지 않는다.
            NeoFunc = NeoNo;
        }
        else if ((String)(const char *)my["device_state"] == "github")
        {
            ota.check();
        }
    }

    Serial.println("Data Change");
    cur = my;
}
