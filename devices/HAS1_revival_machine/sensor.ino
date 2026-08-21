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
// 중거리(2~4cm)가 각각 커버되므로, 감지 실패 시 반대 Gain으로 즉시 한 번 더 시도해
// 근접~4cm 전 구간을 잇는다. TX 출력(GsNOn/CWGsP)은 실측상 기여가 낮아 기본값 유지.
static GainMode currentGain = GAIN_NEAR;

// RFConfiguration(0x32) CfgItem 0x0A(Type A 106kbps Analog Setting)로 RxGain을 전환한다.
// PN532는 이 설정을 내부에 영구 저장하지 않으므로 초기화 때마다(RfidInit) 다시 적용해야 한다.
static bool ApplyGain(int mode)
{
  uint8_t rfCfg = (mode == GAIN_NEAR) ? 0x19 : 0x49;  // 23dB(근거리) / 33dB(중거리)
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
static bool DetectAndRead(uint8_t outData[32])
{
  byte buf[64] = {0};
  if (!nfc.sendCommandCheckAck(buf, 1)) return false;
  if (!nfc.startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A)) return false;
  return nfc.ntag2xx_ReadPage(7, outData);
}

// 현재 Gain으로 실패하면 반대 Gain으로 즉시 재시도. 성공한 Gain은 currentGain에 남아 다음 호출에도 유지된다.
static bool DetectWithGainSwitch(uint8_t outData[32])
{
  if (DetectAndRead(outData)) return true;
  currentGain = (currentGain == GAIN_NEAR) ? GAIN_FAR : GAIN_NEAR;
  ApplyGain(currentGain);
  return DetectAndRead(outData);
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
  currentGain = GAIN_NEAR;
  ApplyGain(currentGain);  // PN532는 RF 설정을 저장하지 않으므로 초기화 때마다 재적용
  Serial.println("RFID 연결성공");
}

/**
 * @brief RFID 태그 인식
 */
void RfidLoop()
{
  if (!rfid_tag)
  {
    rfid_tag = true;
    rfid_timer_id = rfid_timer.setTimeout(1000, RfidTagTimerFunc);
  }
  else
  {
    return;
  }

  uint8_t data[32];
  if (DetectWithGainSwitch(data)) // 근접 Dead Zone 대응 위해 근/원거리 Gain을 자동 전환하며 시도
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

  String game_state_now = (String)(const char *)my["game_state"];

  // setting 상태: 역할 조회 없이, 유효한 형식(G#P#)의 태그면 누구든 태그 즉시 연다.
  if (game_state_now == "setting")
  {
    if (ghost_opened_local || (String)(const char *)my["device_state"] == "open")
    {
      return;  // 이번 라운드에 이미 열림 처리됨 → 중복 태그 무시
    }

    Serial.println("[RFID] Setting tag - opening");

    // 서버 반영 전이라도 로컬 래치로 즉시 잠가 중복 전송을 막는다.
    ghost_opened_local = true;

    has2wifi.Send((String)(const char *)my["device_name"], "device_state", "open");

    NeopixelSet(blue);
    SolenoidPulse();
    NeoFunc = NeoNo;
    return;
  }

  // activate 상태가 아니거나 이미 열린 상태(서버 확정 또는 로컬 래치)면 ghost 태그를 무시한다.
  if (game_state_now != "activate" ||
      (String)(const char *)my["device_state"] == "open" ||
      ghost_opened_local)
  {
    return;
  }

  // 태그한 사용자의 role을 읽어와 ghost 여부 확인.
  // Receive가 성공 여부를 반환하지 않으므로, 매 시도 전 tag를 비우고
  // 응답 device_name이 실제 태그값과 같을 때만 성공으로 판정한다.
  const int tag_lookup_attempts = 3; // 최초 1회 + 재시도 2회
  const int tag_lookup_retry_delay_ms = 200;
  bool tag_lookup_succeeded = false;

  for (int attempt = 1; attempt <= tag_lookup_attempts; attempt++)
  {
    tag.clear();
    Serial.printf("[RFID] Tag lookup attempt %d/%d: %s\n",
                  attempt, tag_lookup_attempts, tagUser.c_str());
    has2wifi.Receive(tagUser);

    const char *received_device_name = (const char *)tag["device_name"];
    if (received_device_name != nullptr && tagUser == received_device_name)
    {
      tag_lookup_succeeded = true;
      Serial.printf("[RFID] Tag lookup success: %s\n", tagUser.c_str());
      break;
    }

    if (received_device_name == nullptr || received_device_name[0] == '\0')
    {
      Serial.println("[RFID] Tag lookup failed: device_name missing");
    }
    else
    {
      Serial.printf("[RFID] Tag lookup mismatch: expected=%s, received=%s\n",
                    tagUser.c_str(), received_device_name);
    }

    if (attempt < tag_lookup_attempts)
      delay(tag_lookup_retry_delay_ms);
  }

  if (!tag_lookup_succeeded)
  {
    Serial.println("[RFID] Tag lookup failed; ghost tag ignored");
    return;
  }

  if ((String)(const char *)tag["role"] != "ghost")
  {
    Serial.println("[RFID] Not a ghost tag; ignored");
    return;
  }

  Serial.println("[RFID] Ghost tagged - opening");

  // 서버 반영 전이라도 로컬 래치로 즉시 잠가 중복 전송을 막는다.
  ghost_opened_local = true;

  has2wifi.Send((String)(const char *)my["device_name"], "device_state", "open");
  has2wifi.Send((String)(const char *)my["device_name"], "game_state", "activate");

  NeopixelSet(blue);   // ghost 태그로 열림 - 네오픽셀 전체 파란색(고정)
  SolenoidPulse();
  NeoFunc = NeoNo;
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

//******************************************* Neopixel Helpers *******************************************
void NeopixelSet(int color[3])
{
  current_neopixel_color = color;
  uint32_t c = Adafruit_NeoPixel::Color(color[0], color[1], color[2]);
  pixels_top.fill(c); pixels_top.show();
  pixels_mid.fill(c); pixels_mid.show();
  pixels_bot.fill(c); pixels_bot.show();
  delay(10);
  pixels_top.show();
  pixels_mid.show();
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

// 평소엔 통전하지 않고, 잠금/해제가 바뀌는 순간에만 SOLENOID_PULSE_MS 동안 짧게 통전한다.
// (래치 없는 솔레노이드를 계속 통전 상태로 유지하면 장시간 발열/소손 위험이 있어 도입)
void SolenoidPulse()
{
  SolenoidOn();
  delay(SOLENOID_PULSE_MS);
  SolenoidOff();
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
