#include "HAS1_revival_machine.h"

void nexInit()
{
}

void sendCommand(const char *cmd)
{
    if (cmd == nullptr)
    {
        return;
    }

    MySerial2.print(cmd);
    MySerial2.write(0xFF);
    MySerial2.write(0xFF);
    MySerial2.write(0xFF);
}

/**
 * @brief 디스플레이에 변화를 주거나 변화가 있을시 실행
 */
void DisplayCheck()
{
    while (MySerial2.available() > 0)
    {
        String nextion_string = MySerial2.readStringUntil((char)0xFF);
        while (MySerial2.available() > 0 && MySerial2.peek() == (char)0xFF)
            MySerial2.read();
        nextion_string.trim();
        if (nextion_string.length() > 0)
            NextionReceived(&nextion_string);
    }
}

/**
 * @brief 디스플레이에서 오는 Serial을 확인
 *
 * @param nextion_string Serial 문자열 데이터
 */
void NextionReceived(String *nextion_string)
{
}

/**
 * @brief get 명령으로 Nextion 숫자 변수를 읽음
 *        성공시 값 반환, 실패시 -1 반환 (0x71 응답 파싱)
 */
int NextionGetNum(const char *cmd)
{
    while (MySerial2.available()) MySerial2.read();  // 이전 잔여 데이터 제거
    sendCommand(cmd);

    uint8_t buf[8] = {0};
    int idx = 0;
    uint32_t timeout = millis() + 500;
    while (millis() < timeout && idx < 8)
    {
        if (MySerial2.available())
            buf[idx++] = MySerial2.read();
    }

    // Nextion 숫자 응답: 0x71 + 4바이트(리틀엔디안) + 0xFF 0xFF 0xFF
    if (idx >= 5 && buf[0] == 0x71)
        return (int32_t)(buf[1] | (buf[2] << 8) | (buf[3] << 16) | (buf[4] << 24));

    return -1;
}

/**
 * @brief 서버 selected_language 와 Nextion vLanguage 를 동기화
 *        값이 다를 때만 Nextion 으로 전송 (0=EN, 1=KO)
 */
void SyncLanguage()
{
    int target = ((String)(const char *)shift_machine["selected_language"] == "EN") ? 0 : 1;
    if (nextion_language != target)
    {
        String cmd = "pgSetting.vLanguage.val=" + String(target);
        sendCommand(cmd.c_str());
        nextion_language = target;
    }
}

/**
 * @brief 초기 연결 후 Nextion 에서 언어/버전 값을 읽어 초기화
 *        - vLanguage: ESP 내부 저장 후 서버와 동기화
 *        - vVersion: 서버 nextion_version 필드에 전송
 */
void NextionInit()
{
    delay(200);

    int lang = NextionGetNum("get pgSetting.vLanguage.val");
    nextion_language = (lang >= 0) ? lang : 1;  // 읽기 실패시 기본값 KO(1)

    int version = NextionGetNum("get pgSetting.vVersion.val");
    if (version >= 0 && (const char *)my["device_name"])
    {
        has2wifi.Send((String)(const char *)my["device_name"],"nextion_version",String(version));
    }

    // ESP32 펌웨어 버전을 서버 device table 의 esp_version 필드로 보고
    if ((const char *)my["device_name"])
    {
        has2wifi.Send((String)(const char *)my["device_name"], "esp_version", String(FIRMWARE_VER));
    }

    SyncLanguage();
}
