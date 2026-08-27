#include "HAS1_altar.h"

//****************************************** Initialize ******************************************
void SensorInit()
{
  // Neopixel init
  pixels_square.begin();
  pixels_round.begin();
  pixels_side.begin();
  pixels_square2.begin();
  pixels_pn532.begin();

  // Rfid init
  RfidInit();

  // Solenoid / IR Sensor / Micro Switch init
  SolenoidInit();
  IrSensorInit();
  MicroSwInit();

  // DFPlayer init (내부에 2초 blocking delay가 있어 맨 마지막에 호출)
  Mp3_Init();
}

//********************************************* Rfid *********************************************
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
  uint8_t uid[] = {0, 0, 0, 0, 0, 0, 0}; // Buffer to store the returned UID
  uint8_t uidLength;                     // Length of the UID (4 or 7 bytes depending on ISO14443A card type)
  uint8_t data[32];
  char user_data[5];
  byte pn532_packetbuffer11[64];
  pn532_packetbuffer11[0] = 0x00;
  BREADCRUMB("RfidLoop:sendCmd");
  if (nfc.sendCommandCheckAck(pn532_packetbuffer11, 1))
  { // rfid 통신 가능한 상태인지 확인
    BREADCRUMB("RfidLoop:detectTarget");
    if (nfc.startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A))
    {                                    // rfid에 tag 찍혔는지 확인용 //데이터 들어오면 uid정보 가져오기
      BREADCRUMB("RfidLoop:readPage");
      if (nfc.ntag2xx_ReadPage(7, data)) // ntag 데이터에 접근해서 불러와서 data행열에 저장
        CardChecking(data);
    }
  }
  BREADCRUMB("RfidLoop:done");
}

/**
 * @brief RFID에 태그된 NFC의 데이터에 따른 코드 동작
 *
 * @param rfidData 태그된 NFC의 데이터
 */
void CardChecking(uint8_t rfidData[32]) // 어떤 카드가 들어왔는지 확인용
{
  BREADCRUMB("CardChecking:recv");
  String tagUser = "";
  static String cur_tag_user = "";
  for (int i = 0; i < 4; i++) // GxPx 데이터만 배열에서 추출해서 string으로 저장
    tagUser += (char)rfidData[i];
  Serial.println("tag_user_data : " + tagUser);

  // 1. 태그한 플레이어의 역할과 생명칩갯수, 최대생명칩갯수 등 읽어오기
  has2wifi.Receive(tagUser);

  // 2. 술래인지, 플레이어인지 구분
  if ((String)(const char *)my["game_state"] == "activate" && (String)(const char *)my["device_state"] == "blink" && (String)(const char *)tag["role"] == "tagger")
  {
    NeoFunc = NeoNo;

    ClearRound();
    pixels_side.clear();
    ClearSquare();
    delay(300);
    lightColor(pixels_round, purple);
    lightColor(pixels_side, purple);
    lightColor(pixels_square, purple);
    delay(300);
    ClearRound();
    pixels_side.clear();
    ClearSquare();
    delay(300);
    lightColor(pixels_round, purple);
    lightColor(pixels_side, purple);
    lightColor(pixels_square, purple);

    BREADCRUMB("CardChecking:taggerSend");
    has2wifi.Send((String)(const char *)my["device_name"], "device_state", "activate");
    has2wifi.Send((String)(const char *)tag["device_name"], "device_state", "activate");

    String tagger_name = (String)(const char *)tag["device_name"];
    String tagger_group = tagger_name.substring(0, 2);
    for (int i = 1; i < 9; ++i)
    {
      // TODO 전체 플레이어에게 술래 정보를 전달
      String player_name = tagger_group + "P" + String(i);
      Serial.println(player_name);
      has2wifi.Send(player_name, "tagger_name", tagger_name);
    }
  }

  if ((String)(const char *)tag["role"] == "tagger" && (int)tag["taken_chip"] > 0)
  {
    // if(RfidNsecTag(2))
    // {
    // 3. 태그한 사용자가 술래이면서 빼앗은 칩을 1개 이상 가지고 있다면
    String altar_state = (String)(const char *)my["device_state"];

    BREADCRUMB("CardChecking:chipSend");
    has2wifi.Send((String)(const char *)my["device_name"], "taken_chip", "+1");
    has2wifi.Send((String)(const char *)tag["device_name"], "taken_chip", "-1");
    has2wifi.Send((String)(const char *)tag["device_name"], "exp", "+100");

    // 애니메이션은 delay()로 막지 않고 NeoFunc()에 맡겨 매 loop마다 조금씩 진행시킨다
    // (그래야 이 태그 처리 중에도 IrSensorLoop/MicroSwLoop가 계속 돌 수 있다).
    if (altar_state == "blink")
    {
      NeoFunc = NeoChipGaugeBlink; // round + square(1,2)가 함께 차오르는 게이지
    }
    else if (altar_state == "activate")
    {
      NeoFunc = NeoChipBlinkActivate; // round + square(1,2)가 깜빡이는 연출
    }
  }
  //}
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
void applyBrightness()
{
  int b = (int)my["brightness"];
  int brightness;
  if (b <= 0 || b > 100)
    brightness = 255;
  else
    brightness = map(b, 0, 100, 0, 255);
  pixels_square.setBrightness(brightness);
  pixels_round.setBrightness(brightness);
  pixels_side.setBrightness(brightness);
  pixels_square2.setBrightness(brightness);
  pixels_pn532.setBrightness(brightness);
  pixels_square.show();
  pixels_round.show();
  pixels_side.show();
  pixels_square2.show();
  pixels_pn532.show();
}

// pixels_square2(두번째 사각 링)/pixels_pn532(태그 지점 링)는 물리적으로 각각
// pixels_square/pixels_round의 짝이라, 기존 호출부를 다 건드리는 대신 헬퍼에서
// 자동으로 같은 색을 미러링한다. clear()는 헬퍼를 안 거치므로 ClearSquare/ClearRound로 대체.
void lightColor(Adafruit_NeoPixel &pixels, int color[3])
{
  pixels.fill(pixels.Color(color[0], color[1], color[2]));
  pixels.show();
  if (&pixels == &pixels_square) lightColor(pixels_square2, color);
  else if (&pixels == &pixels_round) lightColor(pixels_pn532, color);
}

void lightColor(Adafruit_NeoPixel &pixels, int color[3], int index)
{
  pixels.setPixelColor(index, color[0], color[1], color[2]);
  pixels.show();
  if (&pixels == &pixels_round) lightColor(pixels_pn532, color, index % NUMPIXELS_PN532);
  else if (&pixels == &pixels_square) lightColor(pixels_square2, color, index);
}

void ClearSquare()
{
  pixels_square.clear();
  pixels_square2.clear();
}

void ClearRound()
{
  pixels_round.clear();
  pixels_pn532.clear();
}

void lightRgb(Adafruit_NeoPixel &pixels, int r, int g, int b)
{
  pixels.fill(pixels.Color(r, g, b));
  pixels.show();
  if (&pixels == &pixels_square) lightRgb(pixels_square2, r, g, b);
  else if (&pixels == &pixels_round) lightRgb(pixels_pn532, r, g, b);
}

//******************************************* Neopixel *******************************************
void NeoNo()
{
}
// NeoFunc()는 loop()에서 매 반복마다 무조건 호출되므로, 안에서 delay()를 쓰면
// 그 시간만큼 IrSensorLoop/MicroSwLoop가 아예 안 돌아 짧은 스위치 입력을 통째로
// 놓친다. 애니메이션 없는 원색이라 페이싱 자체가 불필요 — 그냥 매번 켠다.
// A 상태 (ready) — 전체 네오픽셀 빨간색
void NeoBeforeTagger()
{
  lightColor(pixels_round, red);  // round + pn532
  lightColor(pixels_side, red);
  lightColor(pixels_square, red); // square + square2
}

void NeoTagger()
{
  ClearRound();
  lightColor(pixels_square, white);
}

void NeoTaggerTag()
{
  static int tag_neo = 0;

  ClearRound();
  pixels_side.clear();

  lightColor(pixels_round, purple, tag_neo);

  if (++tag_neo > NUMPIXELS_ROUND)
  {
    tag_neo = 0;

    ClearRound();
    pixels_side.clear();
    ClearSquare();
  }
}

void NeoAfterTagger()
{
  lightColor(pixels_round, purple);
  // lightColor(pixels_side, purple);
  lightColor(pixels_square, purple);
}

void NeoGaming()
{
  // 애니메이션 없는 단색이라 페이싱 자체가 불필요 — 그냥 매번 켠다.
  lightColor(pixels_round, white);
  lightColor(pixels_square, white);
  // 태그하는 부분(pn532)은 round 색과 무관하게 activate 동안 항상 보라색으로 강조.
  // lightColor(pixels_round, ...)가 자동으로 pn532도 같이 바꿔버리므로 이후에 덮어쓴다.
  lightColor(pixels_pn532, purple);
}

// blink: 생명칩 태그 시 round + square(1,2)가 비율 맞춰 함께 차오르는 게이지.
// delay() 대신 NeoFunc()가 매 loop마다 한 칸씩만 진행시켜 논블로킹으로 동작한다.
void NeoChipGaugeBlink()
{
  static int step = 0;
  static unsigned long next_ms = 0;

  if (millis() < next_ms) return;
  next_ms = millis() + 50;

  if (step == 0)
  {
    pixels_round.clear();
    pixels_square.clear();
    pixels_square2.clear();
  }

  lightColor(pixels_round, purple, step);
  lightColor(pixels_square, purple, step * NUMPIXELS_SQUARE / NUMPIXELS_ROUND);

  if (++step > NUMPIXELS_ROUND)
  {
    step = 0;
    pixels_round.clear();
    pixels_round.show();
    pixels_square.clear();
    pixels_square.show();
    pixels_square2.clear();
    pixels_square2.show();
    NeoFunc = NeoTagger; // blink 상태의 기본 연출로 복귀
  }
}

// activate: 생명칩 태그(+1) 시 round + square(1,2)가 보라색으로 깜빡임.
// pn532(태그 지점)는 이 연출과 무관하게 계속 보라색으로 둔다.
void NeoChipBlinkActivate()
{
  static bool on = false;
  static int blink_count = 0;
  static unsigned long next_ms = 0;

  if (millis() < next_ms) return;
  next_ms = millis() + 150;

  if (on)
  {
    lightColor(pixels_round, purple);
    lightColor(pixels_square, purple);
  }
  else
  {
    // pixels_round/pixels_square를 직접 clear해서(ClearRound/ClearSquare 안 씀)
    // pn532는 건드리지 않고 보라색 그대로 유지한다.
    pixels_round.clear();
    pixels_round.show();
    pixels_square.clear();
    pixels_square.show();
    pixels_square2.clear();
    pixels_square2.show();
  }
  on = !on;

  if (++blink_count >= 6) // on/off 3회
  {
    blink_count = 0;
    on = false;
    NeoFunc = NeoGaming; // activate 기본 연출로 복귀 (pn532도 다시 보라색으로 정리)
  }
}

// void NeoTakenChip()
// {
//   static int chip_neo = 0;

//   if(chip_neo == 0){
//     pixels_side.clear();
//     pixels_square.clear();
//     pixels_round.clear();
//   }

//   pixels_round.lightColor(purple, chip_neo);

//   if(++chip_neo > NUMPIXELS_ROUND){
//     chip_neo = 0;

//     pixels_round.clear();
//     pixels_side.clear();
//     pixels_square.clear();
//   }
// }

void NeoWin()
{
  lightRgb(pixels_round, 0, 0, 20);
  lightRgb(pixels_side, 0, 0, 20);
  lightRgb(pixels_square, 0, 0, 20);
}

void NeoLose()
{
  lightColor(pixels_round, red);
  lightColor(pixels_side, red);
  lightColor(pixels_square, red);
}

//******************************************* Solenoid *******************************************
// 모스펫(IRLZ44N)으로 구동되는 솔레노이드. HIGH = 통전(ON), LOW = 차단(OFF).
// 평소엔 통전하지 않고, "IR센서가 생명칩을 감지한 뒤 마이크로스위치가 눌리는" 시점에만
// 짧게 통전한다(IrSensorLoop가 대기 플래그를 세우고, MicroSwLoop가 소비해서 펄스).
void SolenoidInit()
{
  pinMode(SOLENOID_PIN, OUTPUT);
  digitalWrite(SOLENOID_PIN, LOW);
  solenoid_timer_id = -1;
}

void SolenoidOn()
{
  digitalWrite(SOLENOID_PIN, HIGH);
}

void SolenoidOff()
{
  digitalWrite(SOLENOID_PIN, LOW);
}

void SolenoidPulse()
{
  SolenoidPulse(SOLENOID_PULSE_MS);
}

// delay()로 막으면 열려있는 동안(기본 2초) loop()가 통째로 멈춰서 그 사이 들어오는
// 다음 칩 투입을 못 감지한다 — 타이머로 예약해서 논블로킹으로 닫는다.
void SolenoidPulse(unsigned long ms)
{
  SolenoidOn();
  if (solenoid_timer_id != -1) solenoid_timer.deleteTimer(solenoid_timer_id);
  solenoid_timer_id = solenoid_timer.setTimeout(ms, SolenoidOff);
}

//****************************************** IR Sensor *******************************************
// 생명칩이 투입구를 통과하면 IR_SENSOR가 LOW로 감지됨. 여기서는 솔레노이드를 직접
// 건드리지 않고 "칩이 들어왔다"는 대기 플래그만 세운다 — 실제 개방은 그 다음 마이크로
// 스위치가 눌리는 시점에 MicroSwLoop()가 담당한다.
static bool ir_chip_pending = false;

void IrSensorInit()
{
  pinMode(IR_SENSOR_PIN, INPUT);
}

// "LOW가 N ms 유지돼야 인정" 방식은 접촉이 debounce 시간보다 짧으면 통째로 씹힌다.
// 대신 엣지를 즉시 인정하고, 그 뒤 일정 시간만 재감지를 무시(락아웃)해 채터링만 거른다.
void IrSensorLoop()
{
  static bool stable = false;
  static unsigned long lockout_until_ms = 0;

  if (millis() < lockout_until_ms) return;

  bool raw = (digitalRead(IR_SENSOR_PIN) == LOW);
  if (raw != stable)
  {
    stable = raw;
    lockout_until_ms = millis() + IR_SENSOR_DEBOUNCE_MS;
    if (stable)
    {
      Serial.println("[IrSensor] 생명칩 감지");
      ir_chip_pending = true;
      has2wifi.Send((String)(const char *)my["device_name"], "taken_chip", "+1");
    }
  }
}

//****************************************** Micro Switch *****************************************
// 회전 메커니즘이 한바퀴 돌면 딸깍 눌림 (외부 10K 풀업 → 평소 HIGH, 눌리면 LOW)
void MicroSwInit()
{
  pinMode(MICRO_SW_PIN, INPUT);
}

void MicroSwLoop()
{
  // TODO(임시 디버그): 딸깍이 왜 자주 안 잡히는지 확인용 — 디바운스/락아웃을 거치지 않고
  // GPIO35 raw 값이 실제로 바뀔 때마다 무조건 찍는다. 원인 확인되면 이 블록은 제거할 것.
  {
    static bool debug_last_raw = false;
    bool raw_now = (digitalRead(MICRO_SW_PIN) == LOW);
    if (raw_now != debug_last_raw)
    {
      debug_last_raw = raw_now;
      Serial.printf("[MicroSw][RAW] %s (t=%lu)\n", raw_now ? "LOW(눌림)" : "HIGH(안눌림)", millis());
    }
  }

  static bool stable = false;
  static unsigned long lockout_until_ms = 0;

  if (millis() < lockout_until_ms) return;

  bool raw = (digitalRead(MICRO_SW_PIN) == LOW); // LOW = 눌림
  if (raw != stable)
  {
    stable = raw;
    lockout_until_ms = millis() + MICRO_SW_DEBOUNCE_MS;
    if (stable)
    {
      Serial.println("[MicroSw] 딸깍 감지");
      Mp3PlayLargeFolder(1, 1); // 성공음 (임시 폴더1/파일1 — 실제 SD 구성 확정되면 교체)

      // IR센서로 칩이 들어온 걸 이미 확인했을 때만 솔레노이드를 짧게 연다
      // (game_state 무관 — setting/ready/activate 모두 동일하게 동작).
      // 빈 회전(칩 없이 돌리기)에는 열리지 않는다.
      if (ir_chip_pending)
      {
        Serial.println("[MicroSw] 생명칩 대기 확인 -> 솔레노이드 개방");
        SolenoidPulse();
        ir_chip_pending = false;
      }
    }
  }
}
