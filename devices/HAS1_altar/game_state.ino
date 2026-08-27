#include "HAS1_altar.h"

/**
 * @brief DB gamestate가 setting 일 때 한번동작하는 코드
 */
void SettingFunc()
{
    activate_bool = false;
    NeoFunc = NeoNo;
    lightColor(pixels_round, white);
    lightColor(pixels_side, white);
    lightColor(pixels_square, white);
}

/**
 * @brief DB gamestate가 ready 일 때 한번동작하는 코드
 */

void ReadyFunc()
{
    activate_bool = false;
    NeoFunc = NeoBeforeTagger;
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
        Serial.println("[DataChange] No server data yet, skipping");
        return;
    }

    static StaticJsonDocument<2048> cur;

    bool any_change = false;

    bool brightness_changed = ((int)my["brightness"] != (int)cur["brightness"]);

    if ((String)(const char *)my["game_state"] != (String)(const char *)cur["game_state"])
    {
        Serial.println("[DataChange] game_state: " +
            (String)(const char *)cur["game_state"] + " -> " +
            (String)(const char *)my["game_state"]);
        any_change = true;
        if ((String)(const char *)my["game_state"] == "setting")
        {
            if (brightness_changed) applyBrightness();
            SettingFunc();
        }
        else if ((String)(const char *)my["game_state"] == "ready")
        {
            if (brightness_changed) applyBrightness();
            ReadyFunc();
        }
        else if ((String)(const char *)my["game_state"] == "activate")
        {
            if (brightness_changed) applyBrightness();
            ActivateRunOnce();
        }
    }
    else if (brightness_changed)
    {
        applyBrightness();
    }

    if (brightness_changed)
    {
        Serial.println("[DataChange] brightness: " +
            String((int)cur["brightness"]) + " -> " + String((int)my["brightness"]));
        any_change = true;
    }

    if ((String)(const char *)my["device_state"] != (String)(const char *)cur["device_state"])
    {
        Serial.println("[DataChange] device_state: " +
            (String)(const char *)cur["device_state"] + " -> " +
            (String)(const char *)my["device_state"]);
        any_change = true;
        if ((String)(const char *)my["device_state"] == "activate")
        {
            NeoFunc = NeoGaming;
            // 태그 지점(pn532)은 activate 진입 시 한 번만 보라색으로 고정한다.
            // NeoGaming() 안에서 매 loop마다 다시 켜면(반복 재전송) 깜빡여 보이는
            // 문제가 있어서, round/square 애니메이션과 분리해 여기서 한 번만 설정.
            lightColor(pixels_pn532, purple);
        }
        else if ((String)(const char *)my["device_state"] == "player_win")
        {
            //NeoFunc = NeoLose;
        }
        else if ((String)(const char *)my["device_state"] == "player_lose")
        {
            //NeoFunc = NeoWin;
        }
        else if ((String)(const char *)my["device_state"] == "blink")
        {
            NeoFunc = NeoTagger;
            activate_bool = true;
            // blink 진입 시 side를 보라+흰색 중간톤으로 한 번만 켜둔다(NeoTagger는 side를 안 건드림).
            lightColor(pixels_side, purple_white);
        }
        else if ((String)(const char *)my["device_state"] == "github")
        {
            ota.check();
        }
    }

    if((int)my["taken_chip"] != (int)cur["taken_chip"])
    {
        Serial.println("[DataChange] taken_chip: " +
            String((int)cur["taken_chip"]) + " -> " + String((int)my["taken_chip"]));
        any_change = true;
    }
    if((int)my["max_chip"] != (int)cur["max_chip"])
    {
        Serial.println("[DataChange] max_chip: " +
            String((int)cur["max_chip"]) + " -> " + String((int)my["max_chip"]));
        any_change = true;
    }

    if (any_change)
        Serial.println("[DataChange] applied");

    cur = my;
}