#include "HAS1_duct.h"

void UpdateBrightness()
{
    // 값이 그대로면 건너뛴다 — 변경 감지를 호출부가 아니라 여기서 한다 (전 device 공통 방식).
    // 동결(tagger_mode) 중 바뀐 값도 호출부가 다시 부르는 순간 그대로 반영된다.
    static int prevServerBrightness = -1;  // -1: 첫 호출은 반드시 적용
    int serverBrightness = my["brightness"].as<int>();
    if (serverBrightness == prevServerBrightness) return;
    prevServerBrightness = serverBrightness;

    if (serverBrightness <= 0 || serverBrightness > 100) {
        colorBrightness = DEFAULT_COLOR_BRIGHTNESS;
        lineBrightness  = DEFAULT_LINE_BRIGHTNESS;
    } else {
        colorBrightness = map(serverBrightness, 1, 100, 1, 255);
        lineBrightness  = map(serverBrightness, 1, 100, 1, 255);
    }
    pixels_line.setBrightness(lineBrightness);
    pixels_round.setBrightness(colorBrightness);
    pixels_switch.setBrightness(colorBrightness);
    // setBrightness()만으론 표시가 안 바뀌므로 현재 상태 색을 새 밝기로 다시 칠한다
    ApplyCurrentNeopixel();
}

/**
 * @brief 현재 game_state에 맞게 네오픽셀 색상을 재적용
 */
void ApplyCurrentNeopixel()
{
    if (game_state == setting) {
        pixels_line.lightColor(line_white);
        pixels_round.lightColor(white);
        pixels_switch.lightColor(white);
    } else if (game_state == ready) {
        pixels_line.lightColor(line_red);
        pixels_round.lightColor(red);
        pixels_switch.lightColor(red);
    } else if (game_state == activate) {
        if (duct_available) {
            pixels_line.lightColor(line_yellow);
            pixels_round.lightColor(yellow);
            pixels_switch.lightColor(yellow);
        } else {
            pixels_line.lightColor(line_red);
            pixels_round.lightColor(red);
            pixels_switch.lightColor(red);
        }
    }
}

/**
 * @brief DB gamestate가 setting 일 때 한번 동작하는 코드
 */
void SettingFunc()
{
    game_state = setting;

    use_duct_num = 0;
    duct_available = true;
    current_time = 0;
    cooltime = 0;
    cooltime_timer.deleteTimer(cooltime_timer_id);
    // 이전 게임의 닫기 콜백이 쿨타임이나 관리자 스냅샷을 되살리지 않게 한다.
    duct_close_timer.deleteTimer(duct_close_timer_id);
    digitalWrite(RELAY_PIN, LOW);
    mmmm_open = false;
    switch_available = true;

    pixels_line.lightColor(line_white);
    pixels_round.lightColor(white);
    pixels_switch.lightColor(white);
}

/**
 * @brief DB gamestate가 ready 일 때 한번 동작하는 코드
 */
void ReadyFunc()
{
    game_state = ready;

    use_duct_num = 0;
    duct_available = true;
    current_time = 0;
    cooltime = 0;
    cooltime_timer.deleteTimer(cooltime_timer_id);
    // 이전 게임의 닫기 콜백이 쿨타임이나 관리자 스냅샷을 되살리지 않게 한다.
    duct_close_timer.deleteTimer(duct_close_timer_id);
    digitalWrite(RELAY_PIN, LOW);
    mmmm_open = false;
    switch_available = true;

    pixels_line.lightColor(line_red);
    pixels_round.lightColor(red);
    pixels_switch.lightColor(red);
}

/**
 * @brief DB gamestate가 activate 일 때 반복 동작하는 코드
 */
void ActivateFunc()
{
    static bool switch_was_pressed = false;
    RfidLoop();
    bool switch_pressed = !digitalRead(SW_PIN);
    bool switch_just_pressed = switch_pressed && !switch_was_pressed;
    switch_was_pressed = switch_pressed;
    // 길게 눌러도 안내와 개방은 한 번만 처리한다. 다시 누르려면 버튼을 놓아야 한다.
    if (switch_just_pressed && switch_available) { DuctOpen(true); }
}

/**
 * @brief DB gamestate가 activate 일 때 한번 동작하는 코드
 */
void ActivateRunOnce()
{
    game_state = activate;

    // 쿨타임과 쿨타임 증가량을 DB에서 읽어 사용할 수 있음.
    // cool_time 이 0이거나 아직 안 내려왔으면 쿨타임이 통째로 사라지므로 기본값을 유지한다.
    // cool_time_add 는 0("증가 없음")도 유효한 설정이라 받은 값을 그대로 쓴다.
    int server_cooltime = (int)my["cool_time"];
    if (server_cooltime > 0) cooltime_set = server_cooltime;
    cooltime_add = (int)my["cool_time_add"];

    // 첫 개방이 내부 스위치여도 쿨타임이 0으로 남지 않게 1회차 값을 미리 채운다.
    // (CooltimeTimerFunc 은 current_time >= cooltime 이면 즉시 해제한다)
    cooltime = cooltime_set;

    pixels_line.lightColor(line_yellow);
    pixels_round.lightColor(yellow);
    pixels_switch.lightColor(yellow);
}

/**
 * @brief 주기적으로 DB를 읽어옴
 */
void DataChange()
{
    // 서버 이름만 복사한다. BLE 명령/응답 처리는 loop() 끝에서 진행한다.
    if (my["device_name"].is<const char *>()) {
        Has1BleBeacon::setDeviceName(my["device_name"].as<const char *>());
    }

    // JsonDocument(크기 템플릿 없는 v7 타입) 사용 — StaticJsonDocument<N>은 N이 my와 정확히
    // 같아야만 대입(operator=)이 되는데, 로컬/CI에 깔린 HAS2_Wifi 사본마다 my의 선언 크기가
    // 다를 수 있어(예: 1000 vs 2048) 매번 컴파일 에러가 났다(다른 device들과 동일 이슈).
    static JsonDocument cur;

    // device_state 분기는 항상 처리 (tagger 동결 중에도 back 수신을 감지해야 함)
    if((String)(const char *)my["device_state"] != (String)(const char *)cur["device_state"]){
        if((String)(const char *)my["device_state"] == "github"){
            ota.check();
        }
        else if((String)(const char *)my["device_state"] == "tagger"){
            EnterTaggerMode();
        }
        else if((String)(const char *)my["device_state"] == "back"){
            ExitTaggerMode();
        }
        // 게임 중에는 game_state 가 이미 activate 라 재전송해도 변경 감지에 걸리지 않는다.
        // 봉쇄 해제를 device_state=activate 로도 받을 수 있게 한다(봉쇄 중 device_state 는
        // tagger 이므로 실제로 값이 바뀐다). ExitTaggerMode 가 되보내는 activate 는
        // 이미 cur 에 반영된 뒤라 재진입하지 않는다.
        else if((String)(const char *)my["device_state"] == "activate"){
            ExitTaggerMode();
        }
        else if((String)(const char *)my["device_state"] == "open"){
            MmmmOpen();
        }
    }

    // game_state 전환(setting/ready/activate)은 동결보다 우선한다.
    // 이 함수들이 use_duct_num/duct_available/current_time/cooltime 을 스스로 초기화하므로
    // 동결로 보존할 값 자체가 없고, 반대로 무시하면 봉쇄 중 게임이 리셋됐을 때
    // back 이 오기 전까지(= 다음 게임까지) 보라색 + RFID 차단이 그대로 남는다.
    if ((String)(const char *)my["game_state"] != (String)(const char *)cur["game_state"]){
        if ((String)(const char *)my["game_state"] == "setting"){
            tagger_mode = false;
            SettingFunc();
        }
        else if ((String)(const char *)my["game_state"] == "ready"){
            tagger_mode = false;
            ReadyFunc();
        }
        else if ((String)(const char *)my["game_state"] == "activate"){
            tagger_mode = false;
            ActivateRunOnce();
        }
    }

    // 아래 분기들은 tagger 동결 중에는 무시 (덕트 강제 오픈/밝기 재도색이 동결을 깨뜨림)
    if(!tagger_mode){
        if((String)(const char *)my["manage_state"] != (String)(const char *)cur["manage_state"]){
            if((String)(const char *)my["manage_state"] == "mo"){
                DuctOpen();
            }
        }

        // 밝기 반영 — 변경 감지·재도색은 UpdateBrightness() 내부에서 한다
        UpdateBrightness();
    }

    Serial.println("Data Change");
    cur = my;
}

/**
 * @brief "이로운 효과" 진입 - tagger 수신 시 현 상태와 무관하게 덕트를 동결
 *        (전체 보라색, RFID 비활성, 쿨타임 일시정지). game_state/cool_time은 손대지 않아 보존됨.
 */
void EnterTaggerMode()
{
    if (tagger_mode) return;   // 재진입 방지 (tagger -> activate -> tagger 등)
    tagger_mode = true;        // RfidLoop / CooltimeTimerFunc 자동 정지
    tagger_started_ms = millis();

    // 닫기 예약을 유지해야 일반 쿨타임 시작과 관리자 상태 복원이 빠지지 않는다.
    // 각 닫기 함수가 봉쇄 중 색상과 서버 상태를 보존한다.

    pixels_line.lightColor(line_purple);
    pixels_round.lightColor(purple);
    pixels_switch.lightColor(purple);
    Serial.println("Enter Tagger Mode");
}

/**
 * @brief "이로운 효과" 해제 - back 수신 시 동결을 풀고 원래 game_state 색상으로 복귀.
 *        값들은 동결 중 변하지 않았으므로 별도 복원 없이 재페인트만 수행 (서버 전송 없음).
 */
void ExitTaggerMode()
{
    if (!tagger_mode) return;  // tagger 상태일 때만 동작
    tagger_mode = false;       // RFID / 쿨타임 게이팅 해제 (멈췄던 지점부터 재개)
    ApplyCurrentNeopixel();    // 복원된 game_state / duct_available 기준 색 복원
    Serial.println("Exit Tagger Mode");

    if (game_state == activate)
    {
        if (duct_available)
            has2wifi.Send((String)(const char *)my["device_name"], "device_state", "activate");
        else
            has2wifi.Send((String)(const char *)my["device_name"], "device_state", "lock");
    }
}
