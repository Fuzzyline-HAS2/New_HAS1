#include "HAS1_revival_machine.h"

//****************************************** Initialize ******************************************
void SensorInit()
{
  // Neopixel init
  pixels_top.begin();
  pixels_mid.begin();
  pixels_bot.begin();

  // 초기 전역 밝기 적용 (색상은 풀 밝기로 정의되어 있으므로 여기서 스케일)
  pixels_top.setBrightness(color_brightness);
  pixels_mid.setBrightness(color_brightness);
  pixels_bot.setBrightness(color_brightness);

  // mid는 device_state와 무관하게 항상 흰색 고정 — NeopixelSet()이 top/bot만 칠하므로
  // 여기서 한 번만 켜두면 이후 상태 전환에도 계속 흰색으로 유지된다.
  pixels_mid.fill(Adafruit_NeoPixel::Color(white[0], white[1], white[2]));
  pixels_mid.show();

  // Rfid init
  RfidInit();

  // Solenoid init
  SolenoidInit();
}

//********************************************* Rfid *********************************************
// ── PN532 근접 인식 Dead Zone 대응 — RxGain 동적 전환 (HAS1_generator/HAS1_itembox와 동일 대응) ──
// 일부 생산 로트의 PN532는 기본 RxGain(38dB)에서 태그를 안테나 중심에 맞춰 대면
// 약 2cm 이하 근거리에서 인식이 안 되는 특성이 실측으로 확인됨(로트별 RF 편차,
// MCU/통신 문제 아님). RxGain을 낮추면(23dB) 근거리(~2cm)가, 기본보다 높이면(33dB)
// 중거리(2~4cm)가 각각 커버되나, 완전 밀착(0mm)은 신호가 과도하게 강해 이 두 단계로도
// 안 잡히는 경우가 현장에서 확인됨(밀착 상태로 몇 초씩 붙잡아야 겨우 읽힘). GAIN_CONTACT를
// 추가해 NEAR보다 한 단계 더 낮춘 Gain(약 20dB, 추정치 — 실기 튜닝 필요)으로 밀착 구간을
// 커버한다. 감지 실패 시 나머지 두 Gain을 순서대로 즉시 재시도해 밀착~4cm 전 구간을 잇는다.
// TX 출력(GsNOn/CWGsP)은 실측상 기여가 낮아 기본값 유지.
static GainMode currentGain = GAIN_NEAR;

// RFConfiguration(0x32) CfgItem 0x0A(Type A 106kbps Analog Setting)로 RxGain을 전환한다.
// PN532는 이 설정을 내부에 영구 저장하지 않으므로 초기화 때마다(RfidInit) 다시 적용해야 한다.
static bool ApplyGain(int mode)
{
  // 0x09(약 20dB, 추정) / 0x19(23dB, 근거리) / 0x49(33dB, 중거리). 0x09는 기존 두 값의
  // 비트 패턴(RxGain 필드만 한 단계 낮춤)에서 유추한 추정치라 실기에서 재보정이 필요하다.
  uint8_t rfCfg = (mode == GAIN_CONTACT) ? 0x09 : (mode == GAIN_NEAR) ? 0x19 : 0x49;
  uint8_t cmd[] = {
      0x32,       // RFConfiguration
      0x0A,       // Type A 106kbps Analog Setting
      rfCfg,      // RFCfg — RxGain (아래 TX 관련 값들은 실측상 기본값 유지가 최선이었음)
      0xF4,       // GsNOn
      0x3F,       // CWGsP
      0x11,       // ModGsP
      0x4D,       // Demod RF ON
      0x85,       // RxThreshold
      0x61,       // Demod RF OFF
      0x6F,       // GsNOff
      0x26,       // ModWidth
      0x62,       // MifNFC
      0x87        // TxBitPhase
  };
  return nfc.sendCommandCheckAck(cmd, sizeof(cmd), 1000);
}

// 현재 Gain으로 태그 감지 + page7 읽기를 1회 시도한다.
//
// 예전 시퀀스(0x00 명령 -> startPassiveTargetIDDetection -> ntag2xx_ReadPage)는 PN532 호스트
// 프로토콜에 맞지 않았다(Adafruit PN532 1.3.4 소스로 확인):
//  - sendCommandCheckAck()는 응답이 "준비될 때까지" 기다리기만 하고 읽지 않는다. 그래서
//    InListPassiveTarget 응답을 읽지 않은 채 InDataExchange를 보내면 프레임 위상이 어긋나
//    (다음 명령의 ACK 자리에서 이전 응답을 읽음) 읽기가 실패하거나 이전 데이터가 재사용된다.
//    HAS1_escape_sub d3f0495가 같은 문제를 "응답 drain"으로 고쳤다.
//  - 정의되지 않은 0x00 명령은 에러 프레임(0x7F)만 남긴다. 통신 확인 용도였지만
//    readPassiveTargetID()가 실패로 알려주므로 필요 없다.
//  - 카드가 없을 때 InListPassiveTarget은 기본 재시도(0xFF = 무한)로 끝나지 않는다. 그 상태로
//    다음 명령(ApplyGain)을 보내면 응답을 못 받아 1000ms 타임아웃을 친다(PR #28 실측
//    lastApplyGain=1002ms). RfidInit()의 setPassiveActivationRetries()가 이걸 유한하게 만든다.
// 위상이 어긋난 채 돌다가 우연히 맞을 때만 읽히는 구조라, 카드가 응답하는 타이밍(=거리, 커플링)에
// 따라 성패가 갈렸다 - "밀착하면 안 읽히고 2~3cm 띄우면 읽힌다"가 그 증상이다.
static bool DetectAndRead(uint8_t outData[32])
{
  uint8_t uid[7];
  uint8_t uidLength = 0;
  // InListPassiveTarget을 보내고 응답을 끝까지 읽는다(drain). 카드가 없으면 PN532가
  // RFID_ACTIVATION_RETRIES 회 시도 후 "0 targets"로 스스로 끝내므로 false가 깨끗하게 돌아오고,
  // PN532는 다음 명령을 받을 수 있는 상태로 남는다. timeout은 그 자체 종료가 늦어질 때의 상한이다.
  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, RFID_DETECT_TIMEOUT_MS))
    return false;
  return nfc.ntag2xx_ReadPage(7, outData) != 0;
}

// RFConfiguration(0x32) CfgItem 0x01(RF field)로 필드를 껐다/켠다. ISO14443A 태그는 한 번
// 교신(anti-collision+SELECT)에 성공하면 ACTIVE 상태로 넘어가 이후 REQA(일반 폴링)에
// 응답하지 않는다 - 필드를 벗어나 전원이 끊겨야(또는 명시적 Release/Deselect) 리셋된다.
// 태그를 리더에 계속 붙여둔 채로는 "우연히 리셋될 때"까지 기다리는 수밖에 없었던 게
// 현장 증상(밀착 유지 시 미인식, 짧게 뗐다 대면 즉시 인식)과 들어맞는다. 필드를 잠깐
// 껐다 켜서 강제로 전원을 끊으면, 태그가 실제로 필드를 벗어났다 재진입한 것과 같은
// 효과로 ACTIVE 상태가 풀린다.
static bool ToggleRfField(bool on)
{
  uint8_t cmd[] = {
      0x32,                    // RFConfiguration
      0x01,                    // CfgItem: RF field
      (uint8_t)(on ? 0x01 : 0x00)  // bit0: RF field ON(1)/OFF(0)
  };
  return nfc.sendCommandCheckAck(cmd, sizeof(cmd), 1000);
}

// RF 필드 리셋은 "이미 한 번 인식해서 붙잡고 있던 태그"(gameplay_tag_latched)가 갑자기
// 안 읽힐 때만 쓴다 - ACTIVE 고착을 의심할 근거가 있는 경우다. 태그가 아예 없어서 그냥
// 탐색 중인 상태(대부분의 폴링)까지 매번 리셋하면, 마침 그 순간 사람이 새로 태그를
// 올려놓는 전원 인가 타이밍과 겹쳐 오히려 새 태그 인식을 방해한다(현장: 뗐다 다시
// 대도 여러 번 연속으로 아예 안 읽힘 - tag_user_data조차 안 찍힘). 탐색 중에는 기존처럼
// Gain 순환만 한다. 성공한 Gain은 currentGain에 남아 다음 호출도 그 Gain부터 시도한다.
static bool DetectWithGainSwitch(uint8_t outData[32])
{
  if (DetectAndRead(outData)) return true;

  if (gameplay_tag_latched)
  {
    ToggleRfField(false);
    ToggleRfField(true);
    if (DetectAndRead(outData)) return true;
  }

  static const GainMode kGainOrder[] = {GAIN_CONTACT, GAIN_NEAR, GAIN_FAR};
  for (int i = 0; i < 3; i++)
  {
    if (kGainOrder[i] == currentGain) continue;  // 이미 위에서 시도한 Gain
    currentGain = kGainOrder[i];
    ApplyGain(currentGain);
    if (DetectAndRead(outData)) return true;
  }

  // 붙잡고 있던 태그였다면 다음 폴링 사이클을 위해 한 번 더 리셋해둔다.
  if (gameplay_tag_latched)
  {
    ToggleRfField(false);
    ToggleRfField(true);
  }
  return false;
}

/**
 * @brief RFID(=PN532) 세팅
 */
void RfidInit(void)
{
  nfc.begin(); // nfc 함수 시작
  if (!(nfc.getFirmwareVersion()))
  {
    Serial.println("!!!RFID 연결실패!!! - 계속 진행");
    has2wifi.Send((String)(const char *)my["device_name"], "device_state", "PN532");
    return;
  }
  nfc.SAMConfig(); // configure board to read RFID tags
  // 카드가 없을 때 InListPassiveTarget이 스스로 끝나게 한다(기본 0xFF는 카드가 올 때까지 무한 대기).
  // 이게 없으면 DetectAndRead()가 실패한 뒤의 다음 명령이 바쁜 PN532에 막혀 타임아웃을 친다.
  nfc.setPassiveActivationRetries(RFID_ACTIVATION_RETRIES);
  currentGain = GAIN_NEAR;
  ApplyGain(currentGain);  // PN532는 RF 설정을 저장하지 않으므로 초기화 때마다 재적용
  Serial.println("RFID 연결성공");
}

/**
 * @brief RFID 태그 인식
 */
void RfidLoop()
{
  if (revival_approval_pending)
  {
    AdminCardPollPending();
    return;
  }

  if (!rfid_tag)
  {
    rfid_tag = true;
    rfid_timer_id = rfid_timer.setTimeout(RFID_DEBOUNCE_MS, RfidTagTimerFunc);
  }
  else
  {
    return;
  }

  uint8_t data[32];
  bool detected = DetectWithGainSwitch(data);
  ObserveGameplayTag(detected);
  if (!detected) return;

  // 이미 처리 끝난 같은 태그를 계속 붙잡고 있으면 CardChecking()까지 가지 않는다.
  // (CardChecking() 내부에도 같은 판단이 있지만, 거기서 걸리면 tag_user_data 로그만
  // 반복 찍혀서 "계속 뭔가 시도하다 실패하는" 것처럼 보인다 - 여기서 미리 걸러 조용히 둔다.)
  if (gameplay_tag_latched)
  {
    String heldUser = "";
    for (int i = 0; i < 4; i++) heldUser += (char)data[i];
    if (heldUser == gameplay_tag_user) return;
  }

  CardChecking(data);
}

// 승인 조회를 먼저 처리하고, 조회하지 않는 루프에서만 1초 간격으로 관리자 카드를 확인한다.
// 느린 Gain 재설정/반대 Gain 재시도와 일반 게임 태그 처리는 승인 대기 경로에서 제외한다.
void AdminCardPollPending()
{
  if (revival_approval_polled_this_loop ||
      millis() - revival_approval_last_admin_poll_ms < REVIVAL_ADMIN_POLL_MS) return;

  uint8_t data[32];
  bool detected = DetectAndRead(data);
  revival_approval_last_admin_poll_ms = millis();
  if (!detected) return;
  String tagUser = "";
  for (int i = 0; i < 4; i++) tagUser += (char)data[i];
  if (tagUser == "MMMM") CardChecking(data);
}

/**
 * @brief ready 상태(activate_bool=false) 전용 경량 RFID 폴링.
 *        ready에서는 일반 게임 태그(G#P#)를 받지 않도록 RfidLoop() 자체가 꺼져 있으므로,
 *        MMMM 관리자 카드만은 상태 무관으로 열려야 한다는 요구를 이 함수가 별도로 담당한다.
 *        MMMM이 아니면 CardChecking()으로 넘기지 않고 조용히 무시 — ready의 기존 "태그 비활성"
 *        동작은 MMMM 외에는 그대로 유지된다. rfid_tag/rfid_timer는 RfidLoop()과 동일한 디바운스
 *        상태를 공유하지만, activate_bool로 loop()에서 상호 배타적으로만 호출되므로 안전하다.
 */
void AdminCardPollReady()
{
  if (!rfid_tag)
  {
    rfid_tag = true;
    rfid_timer_id = rfid_timer.setTimeout(RFID_DEBOUNCE_MS, RfidTagTimerFunc);
  }
  else
  {
    return;
  }

  uint8_t data[32];
  if (!DetectWithGainSwitch(data)) return;

  String tagUser = "";
  for (int i = 0; i < 4; i++) tagUser += (char)data[i];
  if (tagUser != "MMMM") return;  // ready 상태에서는 MMMM 외 태그는 계속 무시

  CardChecking(data);
}

/**
 * @brief RFID에 태그된 NFC의 데이터에 따른 코드 동작
 *
 * @param rfidData 태그된 NFC의 데이터
 */
void CardChecking(uint8_t rfidData[32]) // 어떤 카드가 들어왔는지 확인용
{
  String tagUser = "";
  for (int i = 0; i < 4; i++) // GxPx 데이터만 배열에서 추출해서 string으로 저장
    tagUser += (char)rfidData[i];
  Serial.println("tag_user_data : " + tagUser);

  // MMMM 관리자 카드: game_state/device_state(tagger 봉쇄 포함)와 무관하게 최우선으로 항상 연다.
  if (tagUser == "MMMM")
  {
    Serial.println("[RFID] admin card - opening (state-independent)");
    SolenoidPulse(SOLENOID_REVIVAL_PULSE_MS);
    return;
  }

  // 잘못 읽힌 카드가 임의의 서버 key로 전송되지 않도록 G#P# 형식을 검증한다.
  bool valid_tag_user =
      tagUser.length() == 4 &&
      tagUser[0] == 'G' && tagUser[1] >= '0' && tagUser[1] <= '9' &&
      tagUser[2] == 'P' && tagUser[3] >= '0' && tagUser[3] <= '9';
  if (!valid_tag_user)
  {
    Serial.println("[RFID] Invalid tag data (expected G#P#); request skipped");
    return;
  }

  // 역할 조회와 승인 왕복까지 포함해 실제 릴레이 반응시간을 잰다.
  const unsigned long tagDetectedMs = millis();

  String game_state_now = (String)(const char *)my["game_state"];

  if (game_state_now == "activate")
  {
    // 첫 요청의 사용자/계측값을 보존한다. 다른 태그도 승인 대기 중에는 끼어들지 않는다.
    if (revival_approval_pending) return;
    if (gameplay_tag_latched && gameplay_tag_user == tagUser) return;
    gameplay_tag_latched = true;
    gameplay_tag_user = tagUser;
    gameplay_tag_missing = false;
    gameplay_tag_miss_count = 0;
  }

  // tagger(사용 불가) 상태: 생존자든 유령이든 태그하면 역할 상관없이 보라색으로
  // 3번 점멸만 하고(사용 불가 알림) 열리거나 서버로 아무것도 보내지 않는다.
  if ((String)(const char *)my["device_state"] == "tagger")
  {
    Serial.println("[RFID] Tag while device_state=tagger - blink only, no action");
    NeoBlinkPurple(3);
    return;
  }

  // setting 상태: 역할 조회 없이, 유효한 형식(G#P#)의 태그면 누구든 태그할 때마다 연다.
  // 색상은 흰색 그대로 유지하고, device_state/game_state 모두 setting 그대로 둔다(서버 전송 없음).
  // 매번 다시 태그해도 또 열려야 하므로 중복 방지 래치를 걸지 않는다.
  if (game_state_now == "setting")
  {
    Serial.println("[RFID] Setting tag - opening");
    SolenoidPulse();
    return;
  }

  // activate 상태가 아니면(=ready/setting) 역할/서버 확인 없이 5초간 열기만 한다.
  // 게임 세팅을 위해 열어야 할 수도 있기 때문(device_state 무관하게 태그하면 열림).
  if (game_state_now != "activate")
  {
    Serial.println("[RFID] Tag outside gameplay (ready/setting) - opening 5s without role check");
    SolenoidPulse(SOLENOID_REVIVAL_PULSE_MS);
    return;
  }

  // 생명장치가 이미 열린 상태(서버 확정, device_state=="open")에서는 유령만 다시 열 수 있다.
  // 원래는 생존자가 태그해도 무조건 다시 열렸지만, open 이후 재입장은 유령 전용으로 제한한다.
  if ((String)(const char *)my["device_state"] == "open")
  {
    has2wifi.Receive(tagUser);
    String role = (String)(const char *)tag["role"];
    if (role == "ghost")
    {
      Serial.println("[RFID] Tag on already-open revival machine - ghost role, opening 5s");
      SolenoidPulse(SOLENOID_REVIVAL_PULSE_MS);
    }
    else
    {
      Serial.println("[RFID] Tag on already-open revival machine - non-ghost role(" + role + "), ignored");
    }
    return;
  }

  // is_open은 생명장치가 아니라 태그한 iotGlove 쪽 필드. 이미 true면(이 iotGlove가
  // 생명장치를 이미 연 적 있으면) 서버로 보내지 않고 사용 불가로 처리한다.
  // device_state=="tagger"와 달리 여기서는 점멸 후에도 device_state가 계속 "activate"라
  // NeoBlinkPurple만 쓰면 노란색(activate)으로 안 돌아오고 빨간색 점멸의 마지막 색에 머무르게 되므로 복원한다.
  const unsigned long roleReceiveStartMs = millis();
  has2wifi.Receive(tagUser);
  const unsigned long roleReceiveMs = millis() - roleReceiveStartMs;
  String tag_role = (String)(const char *)tag["role"];
  // roleReceiveMs는 role=="ghost"일 때만 [GhostTiming] 로그로 남았다 - 이 자리에서 매번
  // 찍어야 "태그 직후 지연이 Receive() HTTP 왕복 때문"인지 다른 role에서도 확인 가능하다.
  Serial.println("[RFID] " + tagUser + " is_open=" + String((int)tag["is_open"]) + " role=" + tag_role +
                 " (Receive took=" + String(roleReceiveMs) + "ms)");
  if ((int)tag["is_open"] != 0)
  {
    Serial.println("[RFID] iotGlove is_open=true - blink only, no action: " + tagUser);
    NeoBlinkPurple(3);
    NeopixelSet(yellow);  // activate 상태 색으로 복원
    return;
  }

  // 활성화 여부/역할(ghost·revival)/쿨다운/최초사용 여부는 모두 서버가 판단한다.
  // 이 기기는 태그 이벤트만 전달하고, 통과 시 device_state="open"이 폴링으로
  // 내려올 때(game_state.ino DataChange)에야 실제로 문을 연다.
  Serial.println("[RFID] Tag detected - sending situation to server: " + tagUser);
  last_open_tag_user = tagUser;  // DataChange()에서 open 확정 시 이 iotGlove의 is_open을 true로 쓰기 위해 기억

  // 유령 태그 -> 실제 릴레이 HIGH까지 측정. 사용자 조회 이전 시각을 사용하고,
  // game_state.ino에서 SolenoidPulse가 반환하는 HIGH 시각으로 종료한다.
  // role=="ghost"일 때만 기록한다 - 생존자/술래가 태그해도 서버가 open을 안 주는 게
  // 정상 동작이라, 이 경우까지 재면 매번 타임아웃으로 잡혀 로그가 오염된다.
  if (tag_role == "ghost")
  {
    ghost_tag_start_ms = tagDetectedMs;
    ghost_role_receive_ms = roleReceiveMs;
    ghost_poll_count = 0;
    ghost_rssi_at_tag = WiFi.RSSI();
    ghost_open_pending = true;
  }
  else
  {
    Serial.println("[GhostTiming] skip - role=" + tag_role + " (not ghost, open not expected)");
  }

  BeginRevivalApproval(tagDetectedMs);
  unsigned long situationStartMs = millis();
  bool situation_sent = has2wifi.Situation(tagUser, "revival_machine");
  ghost_situation_ms = millis() - situationStartMs;
  Serial.println("[RFID] Situation send " + tagUser + " result=" + String(situation_sent ? "OK" : "FAIL") +
                 " took=" + String(ghost_situation_ms) + "ms");

  // HTTP 200은 승인 자체가 아니다. 즉시 상태를 읽고, 미확정이면 전용 폴링으로 계속 확인한다.
  // API가 거부 사유를 노출하지 않으므로 상태가 그대로인 거부도 15초 상한으로 끝낸다.
  if (situation_sent)
  {
    PollRevivalApproval();
    // 서버 계약상 유령 외 역할은 개방 대상이 아니다. 이벤트/즉시 조회는 유지하되
    // 거부된 생존자 태그가 다음 유령의 사용을 15초 동안 막지 않도록 대기를 끝낸다.
    if (tag_role != "ghost")
    {
      EndRevivalApproval("role not eligible");
      // 서버까지 다녀왔지만 유령이 아니라 거부됐다는 걸 태그한 사람에게 알려준다.
      // device_state는 여기까지 오면 "activate"이므로 NeoBlinkPurple이 빨간색으로 점멸한다.
      NeoBlinkPurple(3);
      NeopixelSet(yellow);  // activate 상태 색으로 복원
    }
  }
  else
    EndRevivalApproval("Situation failed", tag_role == "ghost");
}

bool RfidNsecTag(int sec)
{
  if (nsec_tag_num == 0 && !nsec_tag_bool)
  {
    nsec_tag_timer_id = nsec_tag_timer.setTimeout(5000, NsecTagTimerFailFunc);
    nsec_tag_bool = true;
  }
  else
  {
    nsec_tag_timer.restartTimer(nsec_tag_timer_id);
  }

  if (nsec_tag_num >= sec && nsec_tag_bool)
  {
    Serial.println("태그 성공");
    nsec_tag_timer.deleteTimer(nsec_tag_timer_id);
    nsec_tag_bool = false;
    nsec_tag_timer_id = nsec_tag_timer.setTimeout(2000, NsecTagTimerSuccessFunc);
    return true;
  }
  else
  {
    nsec_tag_num++;
  }
  return false;
}

// "사용 불가" 알림으로 짧게 점멸시킨다. device_state=="tagger"면 그 상태의 기본 색(보라색)으로,
// 그 외(예: activate 중 is_open 차단)는 빨간색으로 점멸한다. 점멸 후 상태 복원은 호출부 책임.
void NeoBlinkPurple(int times)
{
  int neoOff[3] = {0, 0, 0};
  int* blinkColor = ((String)(const char *)my["device_state"] == "tagger") ? purple : red;
  for (int i = 0; i < times; i++)
  {
    NeopixelSet(neoOff);
    delay(150);
    NeopixelSet(blinkColor);
    delay(150);
  }
}

//******************************************* Neopixel Helpers *******************************************
void NeopixelSet(int color[3])
{
  // mid는 건드리지 않는다 — device_state와 무관하게 항상 흰색 고정(SensorInit 참고).
  current_neopixel_color = color;
  uint32_t c = Adafruit_NeoPixel::Color(color[0], color[1], color[2]);
  pixels_top.fill(c); pixels_top.show();
  pixels_bot.fill(c); pixels_bot.show();
  delay(10);
  pixels_top.show();
  pixels_bot.show();
}

void ApplyBrightness(int raw)
{
  // raw: 0~255 전역 밝기. 색 배열은 풀 밝기(255)로 두고 setBrightness()로만 스케일.
  color_brightness = raw;
  pixels_top.setBrightness(raw);
  pixels_mid.setBrightness(raw);
  pixels_bot.setBrightness(raw);
  // 현재 켜져 있는 색을 새 밝기로 즉시 반영
  pixels_top.show();
  pixels_mid.show();
  pixels_bot.show();
}

void SetBrightness(int pct)
{
  // 값이 그대로면 건너뛴다 — 변경 감지를 호출부가 아니라 여기서 한다 (전 device 공통 방식).
  static int prevServerBrightness = -1;  // -1: 첫 호출은 반드시 적용
  if (pct == prevServerBrightness) return;
  prevServerBrightness = pct;

  int raw;
  if (pct <= 0 || pct > 100)
    raw = DEFAULT_BRIGHTNESS;
  else
    raw = map(pct, 1, 100, 1, 255);
  ApplyBrightness(raw);
}

void lightColor(Adafruit_NeoPixel &pixels, int color[3], int index)
{
  pixels.setPixelColor(index, color[0], color[1], color[2]);
  pixels.show();
}

//******************************************* Solenoid *******************************************
// 모스펫으로 구동되는 솔레노이드. HIGH = 통전(ON), LOW = 차단(OFF)로 가정.
// (배선이 반대라면 SolenoidOn/Off의 HIGH/LOW만 뒤집으면 됨)
void SolenoidInit()
{
  pinMode(SOLENOID_PIN, OUTPUT);
  digitalWrite(SOLENOID_PIN, LOW);
}

void SolenoidOn()
{
  digitalWrite(SOLENOID_PIN, HIGH);
}

void SolenoidOff()
{
  digitalWrite(SOLENOID_PIN, LOW);
}

// 평소엔 통전하지 않고, 잠금/해제가 바뀌는 순간에만 짧게 통전한다.
// (래치 없는 솔레노이드를 계속 통전 상태로 유지하면 장시간 발열/소손 위험이 있어 도입)
void SolenoidPulse()
{
  SolenoidPulse(SOLENOID_PULSE_MS);
}

unsigned long SolenoidPulse(unsigned long ms)
{
  SolenoidOn();
  const unsigned long relayOnMs = millis();
  delay(ms);
  SolenoidOff();
  return relayOnMs;
}

//******************************************* Neopixel *******************************************
void NeoNo()
{
}
// A 상태
void NeoBeforeTagger()
{
  delay(100);
  static int breathe = 0;
  static bool breathe_direction = true;

  breathe += breathe_direction ? BREATHE_STEP : -BREATHE_STEP;
  if (breathe >= BREATHE_MAX) { breathe = BREATHE_MAX; breathe_direction = false; }
  else if (breathe <= 0)      { breathe = 0;           breathe_direction = true;  }

  pixels_mid.fill(Adafruit_NeoPixel::Color(breathe, 0, 0)); pixels_mid.show();
  pixels_bot.fill(Adafruit_NeoPixel::Color(breathe, 0, 0)); pixels_bot.show();
  pixels_top.fill(Adafruit_NeoPixel::Color(red[0], red[1], red[2])); pixels_top.show();
}

void NeoTagger()
{
  delay(100);
  static int breathe_2 = 0;
  static bool breathe_direction_2 = true;

  breathe_2 += breathe_direction_2 ? BREATHE_STEP : -BREATHE_STEP;
  if (breathe_2 >= BREATHE_MAX) { breathe_2 = BREATHE_MAX; breathe_direction_2 = false; }
  else if (breathe_2 <= 0)      { breathe_2 = 0;           breathe_direction_2 = true;  }

  pixels_bot.fill(Adafruit_NeoPixel::Color(breathe_2, breathe_2, breathe_2)); pixels_bot.show();

  pixels_mid.clear();
  NeoArrow();
}

void NeoTaggerTag()
{
  static int tag_neo = 0;

  pixels_mid.clear();
  pixels_bot.clear();

  lightColor(pixels_mid, purple, tag_neo);

  if (++tag_neo > NUMPIXELS_MID)
  {
    tag_neo = 0;

    pixels_mid.clear();
    pixels_bot.clear();
    pixels_top.clear();
  }
}

void NeoAfterTagger()
{
  static bool after_tagger_neo_bool = false;

  if (after_tagger_neo_bool)
  {
    after_tagger_neo_bool = false;
    pixels_mid.clear();
    pixels_bot.clear();
    pixels_top.clear();
  }
  else
  {
    after_tagger_neo_bool = true;
    NeopixelSet(purple);
  }
}

void NeoGaming()
{
  delay(100);
  static int breathe = 0;
  static bool breathe_direction = true;

  breathe += breathe_direction ? BREATHE_STEP : -BREATHE_STEP;
  if (breathe >= BREATHE_MAX) { breathe = BREATHE_MAX; breathe_direction = false; }
  else if (breathe <= 0)      { breathe = 0;           breathe_direction = true;  }

  pixels_mid.fill(Adafruit_NeoPixel::Color(breathe, 0, breathe)); pixels_mid.show();
  pixels_bot.fill(Adafruit_NeoPixel::Color(breathe, 0, breathe)); pixels_bot.show();
  NeoArrow();
}

// void NeoTakenChip()
// {
//   static int chip_neo = 0;

//   if(chip_neo == 0){
//     pixels_bot.clear();
//     pixels_top.clear();
//     pixels_mid.clear();
//   }

//   pixels_mid.lightColor(purple, chip_neo);

//   if(++chip_neo > NUMPIXELS_MID){
//     chip_neo = 0;

//     pixels_mid.clear();
//     pixels_bot.clear();
//     pixels_top.clear();
//   }
// }

void NeoWin()
{
  static bool win_neo_bool = false;
  static int win_neo = 255;   // 풀 밝기 파랑, 실제 밝기는 setBrightness()가 결정
  static int win_neo_delay = 1500;

  win_neo_delay = win_neo_delay - 100;

  if (win_neo_bool)
  {
    win_neo_bool = false;
    int win_color[3] = {0, 0, win_neo};
    NeopixelSet(win_color);
  }
  else
  {
    win_neo_bool = true;
    pixels_mid.clear();
    pixels_bot.clear();
    pixels_top.clear();
  }

  if (win_neo_delay <= 300)
  {
    pixels_mid.clear();
    pixels_bot.clear();
    pixels_top.clear();

    NeoFunc = NeoNo;
  }
  delay(win_neo_delay);
}

void NeoLose()
{
  static bool lose_neo_bool = false;
  static int lose_neo = 255;   // 풀 밝기 빨강, 실제 밝기는 setBrightness()가 결정
  static int lose_neo_delay = 1500;

  lose_neo_delay = lose_neo_delay - 100;

  // 깜빡임을 표현
  if (lose_neo_bool)
  {
    lose_neo_bool = false;
    int lose_color[3] = {lose_neo, 0, 0};
    NeopixelSet(lose_color);
  }
  else
  {
    lose_neo_bool = true;
    pixels_mid.clear();
    pixels_bot.clear();
    pixels_top.clear();
  }

  if (lose_neo_delay <= 300)
  {
    pixels_mid.clear();
    pixels_bot.clear();
    pixels_top.clear();

    NeoFunc = NeoNo;
  }
  delay(lose_neo_delay);
}

void NeoArrow()
{
  static int arrow_pattern = 0;

  switch (arrow_pattern)
  {
  case 0:
    pixels_top.clear();
    break;

  case 1:
    arrow_neo_line_1 = 0;
    arrow_neo_line_2 = 16;
    arrow_neo_line_3 = 0;
    break;

  case 2:
    arrow_neo_line_1 = 1;
    arrow_neo_line_2 = 24;
    arrow_neo_line_3 = 1;
    break;

  case 3:
    arrow_neo_line_1 = 3;
    arrow_neo_line_2 = 12;
    arrow_neo_line_3 = 3;
    break;

  case 4:
    arrow_neo_line_1 = 6;
    arrow_neo_line_2 = 6;
    arrow_neo_line_3 = 6;
    break;

  case 5:
    arrow_neo_line_1 = 12;
    arrow_neo_line_2 = 3;
    arrow_neo_line_3 = 12;
    break;

  case 6:
    arrow_neo_line_1 = 24;
    arrow_neo_line_2 = 1;
    arrow_neo_line_3 = 24;
    break;

  case 7:
    arrow_neo_line_1 = 16;
    arrow_neo_line_2 = 0;
    arrow_neo_line_3 = 16;
    break;

  default:
    break;
  }

  if (++arrow_pattern > 7)
  {
    arrow_pattern = 0;
  }

  NeoArrowSet(1, arrow_neo_line_1);
  // NeoArrowSet(2, arrow_neo_line_2);
  NeoArrowSet(3, arrow_neo_line_3);
  pixels_top.show();
}

void NeoArrowSet(int arrow_neo_line_num, int arrow_neo_line)
{
  int neo_num = 0;

  if (arrow_neo_line_num == 1)
  {
    neo_num = 0;
  }
  else if (arrow_neo_line_num == 2)
  {
    neo_num = 5;
  }
  else if (arrow_neo_line_num == 3)
  {
    neo_num = 10;
  }

  switch (arrow_neo_line)
  {
  case 0:
    pixels_top.setPixelColor(neo_num + 1, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 2, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 3, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 4, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 5, 0, 0, 0);
    break;
  case 1:
    pixels_top.setPixelColor(neo_num + 1, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 2, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 3, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 4, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 5, 255, 0, 255);
    break;
  case 3:
    pixels_top.setPixelColor(neo_num + 1, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 2, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 3, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 4, 255, 0, 255);
    pixels_top.setPixelColor(neo_num + 5, 255, 0, 255);
    break;
  case 6:
    pixels_top.setPixelColor(neo_num + 1, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 2, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 3, 255, 0, 255);
    pixels_top.setPixelColor(neo_num + 4, 255, 0, 255);
    pixels_top.setPixelColor(neo_num + 5, 0, 0, 0);
    break;
  case 12:
    pixels_top.setPixelColor(neo_num + 1, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 2, 255, 0, 255);
    pixels_top.setPixelColor(neo_num + 3, 255, 0, 255);
    pixels_top.setPixelColor(neo_num + 4, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 5, 0, 0, 0);
    break;
  case 24:
    pixels_top.setPixelColor(neo_num + 1, 255, 0, 255);
    pixels_top.setPixelColor(neo_num + 2, 255, 0, 255);
    pixels_top.setPixelColor(neo_num + 3, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 4, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 5, 0, 0, 0);
    break;
  case 16:
    pixels_top.setPixelColor(neo_num + 1, 255, 0, 255);
    pixels_top.setPixelColor(neo_num + 2, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 3, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 4, 0, 0, 0);
    pixels_top.setPixelColor(neo_num + 5, 0, 0, 0);
    break;
  default:
    break;
  }
}
