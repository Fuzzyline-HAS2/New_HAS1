#include "HAS1_revival_machine.h"

/**
 * @brief DB gamestate가 setting 일 때 한번동작하는 코드
 */
void SettingFunc()
{
    activate_bool = false;
    sendCommand("page pgSetting");
    NeoFunc = NeoNo;
    NeopixelSet(white);
}

/**
 * @brief DB gamestate가 ready 일 때 한번동작하는 코드
 */

void ReadyFunc()
{
    activate_bool = false;
    sendCommand("page pgInfo");
    NeopixelSet(red);   // ready - 네오픽셀 전체 빨간색(고정)
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

    static StaticJsonDocument<2048> cur;

    String cmd;

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
            altar_used_local = false;   // 제단 재활성화 → 로컬 잠금 해제(다음 봉헌 허용)
            sendCommand("page pgChipCount");
            NeopixelSet(purple);   // activate - 네오픽셀 전체 보라색(고정)
            NeoFunc = NeoNo;       // 호흡 애니메이션 없음
        }
        else if ((String)(const char *)my["device_state"] == "used")
        {
            // 현재 페이지가 pgUsed 가 아니면 강제 전환 (Nextion 내부 타이머 누락 대비)
            if (NextionGetNum("get dp") != PG_USED_ID)
                sendCommand("page pgUsed");
            NeopixelSet(red);   // 칩 사용됨 - 네오픽셀 전체 빨간색
            NeoFunc = NeoNo;    // 애니메이션 정지(빨간색 고정 유지)
        }
        else if ((String)(const char *)my["device_state"] == "player_win")
        {
            sendCommand("page pgTaggerLose");
            NeopixelSet(red);   // player_win(=tagger 패배) - 네오픽셀 전체 빨간색(고정)
            NeoFunc = NeoNo;    // 깜빡임 애니메이션 없음
        }
        else if ((String)(const char *)my["device_state"] == "player_lose")
        {
            sendCommand("page pgTaggerWin");
            NeopixelSet(blue);  // player_lose(=tagger 승리) - 네오픽셀 전체 파란색(고정)
            NeoFunc = NeoNo;    // 깜빡임 애니메이션 없음
        }
        else if ((String)(const char *)my["device_state"] == "blink")
        {
            if (rfid_tag) return;
            sendCommand("page pgAltarTag");
            NeopixelSet(red);   // blink - 네오픽셀 전체 빨간색(고정)
            NeoFunc = NeoNo;    // 호흡 애니메이션 없음
        }
        else if ((String)(const char *)my["device_state"] == "github")
        {
            ota.check();
        }
    }

    if((int)my["taken_chip"] != (int)cur["taken_chip"])
    {
        // 전역변수 갱신만 하면 pgUsed가 자동으로 받아서 표시.
        // 페이지 전환은 Nextion 내부 타이머가 담당.
        cmd = "pgChipCount.vSacrificeChip.val=" + (String)(int)my["taken_chip"];
        sendCommand(cmd.c_str());
    }
    if((int)my["max_chip"] != (int)cur["max_chip"])
    {
        cmd = "pgChipCount.vMaxChip.val=" + (String)(int)my["max_chip"];
        sendCommand(cmd.c_str());
    }
    SyncLanguage();

    Serial.println("Data Change");
    cur = my;
}
