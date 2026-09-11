#include "HAS1_altar.h"

// 술래 활성화(pn532 태그) + 생명칩 투입(IR센서), 둘 다 충족돼야 발동. 어느 쪽이
// 먼저든 상관없이(RFID는 ~1초마다만 폴링되므로 IR감지가 먼저 올 수도 있다) 나중에
// 확인되는 쪽에서 성공 처리한다. device_state는 항상 "activate"로 고정 - 여기서
// 바꾸지 않는다. (RfidLoop/CardChecking보다 먼저 선언돼야 함 - Arduino는 함수
// 프로토타입만 자동 생성하고 변수는 안 해줘서, 사용부보다 파일 앞쪽에 있어야 한다.)
static bool tag_active = false;
static unsigned long tag_start_time = 0;
static String pending_tagger_device_name = "";
static bool ir_chip_pending = false;          // IR센서가 칩을 감지해서 크랭크 대기 중
static bool ir_chip_confirm_animated = false; // 이 칩(ir_chip_pending 세션)에 대해 OnTagChipConfirmed(애니메이션)를 이미 실행했는지
static bool pending_success_sound = false;    // (1,1) 성공음을 마이크로스위치 클릭까지 대기
// 술래가 훔친 칩을 반납하는 기존 메커닉(아래 두번째 분기)도 태그가 붙어있는 동안
// 디바운스 없이 매초(RfidLoop 폴링 주기) 블로킹 HTTP 3개(has2wifi.Send)를 반복
// 발사했었다 - 그동안 loop()가 막혀 MicroSw 클릭을 놓치는 원인이 됐다. 한 번만
// 처리하도록 tag_active와 같은 방식으로 게이트.
static bool chip_return_processed = false;

// 태그+칩이 (순서 상관없이) 둘 다 확인된 순간 - 로컬 애니메이션만 즉시 트리거해서
// "확인됐으니 크랭크를 돌리라"는 피드백을 준다. taken_chip 갱신/tagger_name
// 브로드캐스트(서버 반영)는 여기서 하지 않는다 - 태그+칩만 확인되고 크랭크를 안
// 돌리면 점수가 올라가면 안 되므로, 실제 반영은 RunAltarSuccess()가 맡고 그건
// MicroSwLoop가 솔레노이드를 실제로 여는 순간에만 호출한다.
void OnTagChipConfirmed()
{
  Serial.println("[Altar] Tag + chip confirmed together - waiting for crank");
  // 애니메이션은 delay() 없이 NeoFunc()가 매 loop마다 조금씩 진행시킨다.
  NeoFunc = NeoChipBlinkActivate;
}

// 크랭크가 실제로 돌아가 솔레노이드가 열리는 순간(MicroSwLoop)에만 호출 -
// taken_chip 갱신으로 서버에 확정 반영한다. (tagger_name 전체 브로드캐스트는
// 더 이상 쓰지 않아 제거 - 플레이어 8명한테 매번 블로킹 HTTP를 쐈었음.)
void RunAltarSuccess()
{
  Serial.println("[Altar] Crank turned -> success committed to server");
  has2wifi.Send((String)(const char *)my["device_name"], "taken_chip", "+1");

  // (1,1) 성공음은 activate(생명칩 바치는 루틴)일 때만 - blink(술래 활성화)에서는
  // 마이크로스위치가 눌려도 재생하지 않는다.
  if ((String)(const char *)my["device_state"] == "activate")
  {
    pending_success_sound = true;
  }
}

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
    Serial.println("!!!RFID connect failed!!! - continuing anyway");
    has2wifi.Send((String)(const char *)my["device_name"], "device_state", "PN532");
    return;
  }
  nfc.SAMConfig(); // configure board to read RFID tags
  Serial.println("RFID connected successfully");
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
  byte pn532_packetbuffer11[64];
  pn532_packetbuffer11[0] = 0x00;
  BREADCRUMB("RfidLoop:sendCmd");
  bool tag_present = false;
  if (nfc.sendCommandCheckAck(pn532_packetbuffer11, 1))
  { // rfid 통신 가능한 상태인지 확인
    BREADCRUMB("RfidLoop:detectTarget");
    if (nfc.startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A))
    {                                    // rfid에 tag 찍혔는지 확인용 //데이터 들어오면 uid정보 가져오기
      BREADCRUMB("RfidLoop:readPage");
      if (nfc.ntag2xx_ReadPage(7, data)) // ntag 데이터에 접근해서 불러와서 data행열에 저장
      {
        tag_present = true;
        CardChecking(data);
      }
    }
  }

  // Tagger left the reader without inserting a chip - reset and ignore.
  if (!tag_present && tag_active)
  {
    Serial.println("[RFID] Tag left reader - reset, ignored");
    tag_active = false;
  }
  // 태그가 리더에서 떠났으면 반납 처리 게이트도 같이 리셋 (다음에 다시 태그하면 재처리 가능해야 함).
  if (!tag_present)
  {
    chip_return_processed = false;
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
  for (int i = 0; i < 4; i++) // GxPx 데이터만 배열에서 추출해서 string으로 저장
    tagUser += (char)rfidData[i];
  Serial.println("tag_user_data : " + tagUser);

  // 1. 태그한 플레이어의 역할과 생명칩갯수, 최대생명칩갯수 등 읽어오기
  has2wifi.Receive(tagUser);

  // 2. 술래 태그 확인 - device_state/서버 호출 없이 내부 플래그만 세운다.
  // 태그가 계속 붙어있는 동안 반복 감지돼도(리더가 ~1초마다 다시 읽음) 한 번만 처리.
  if ((String)(const char *)my["game_state"] == "activate" && (String)(const char *)tag["role"] == "tagger")
  {
    if (!tag_active)
    {
      tag_active = true;
      tag_start_time = millis();
      pending_tagger_device_name = (String)(const char *)tag["device_name"];
      Serial.println("[Altar] Tagger tag detected, waiting for chip insertion: " + pending_tagger_device_name);

      // 칩이 태그보다 먼저 감지돼서 이미 대기 중이었다면(RFID는 ~1초마다만 폴링돼서
      // IR감지가 먼저 올 수 있음), 지금 태그로 뒤늦게 완성된 것 - 확인 애니메이션만
      // 트리거한다(서버 반영은 크랭크가 실제로 돌아갈 때 RunAltarSuccess가 처리).
      // (game_state=="activate"는 이미 위에서 확인됨 - device_state는 blink든
      // activate든 상관없이 태그+칩+크랭크면 열려야 한다.)
      // tag_active는 여기서 false로 되돌리지 않는다 - 태그는 여전히 리더에 붙어있고,
      // 되돌리면 다음 폴링(~1초 뒤)에 "새로 태그됨"으로 오인해서 애니메이션이
      // 매초 반복 트리거되는 버그가 있었다.
      if (ir_chip_pending && !ir_chip_confirm_animated)
      {
        ir_chip_confirm_animated = true;
        OnTagChipConfirmed();
      }
    }
    // already tag_active -> do nothing, avoid re-running while tag stays on reader
  }

  if (!chip_return_processed && (String)(const char *)tag["role"] == "tagger" && (int)tag["taken_chip"] > 0)
  {
    // if(RfidNsecTag(2))
    // {
    // 3. 태그한 사용자가 술래이면서 빼앗은 칩을 1개 이상 가지고 있다면
    chip_return_processed = true; // 태그가 붙어있는 동안 반복 실행(블로킹 HTTP 반복 발사) 방지
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
    Serial.println("Tag success");
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
  // ClearRound()는 버퍼만 지우고 show()를 안 불러서 이전 색(ready의 빨간색)이
  // 화면에 그대로 남아있었다 - 흰색을 명시적으로 켜서 실제로 반영되게 한다.
  lightColor(pixels_round, white);  // round + pn532
  lightColor(pixels_square, white); // square + square2
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
  // round/square(1,2)는 taken_chip/max_chip 비율에 따라 연한 보라(적게 모음)에서
  // 진한 보라(많이 모음)로 표시. max_chip 데이터가 아직 없거나(부팅 직후 등)
  // 0 이하면 기본 보라색(purple[3]과 동일한 밝기)으로 둔다.
  int taken = (int)my["taken_chip"];
  int max_chip = (int)my["max_chip"];
  int mag;
  if (max_chip <= 0)
  {
    mag = 20;
  }
  else
  {
    float ratio = (float)taken / (float)max_chip;
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;
    mag = GAUGE_PURPLE_MIN + (int)((GAUGE_PURPLE_MAX - GAUGE_PURPLE_MIN) * ratio);
  }
  int gauge_purple[3] = { mag, 0, mag };

  // pixels_round는 lightColor()를 거치면 pn532까지 매번 덮어써버린다(pn532는
  // DataChange()에서 activate 진입 시 한 번만 고정해두는 것으로 바꿨음) — 그래서
  // 여기서는 round만 직접 갱신하고 pn532는 건드리지 않는다.
  pixels_round.fill(pixels_round.Color(gauge_purple[0], gauge_purple[1], gauge_purple[2]));
  pixels_round.show();
  lightColor(pixels_square, gauge_purple); // square + square2
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
// 생명칩이 투입구를 통과하면 IR_SENSOR가 LOW로 감지됨. ir_chip_pending을 세우고,
// 이미 태그가 확인된 상태(tag_active)면 태그+칩이 (이 순서로) 동시에 확인된
// 것이므로 OnTagChipConfirmed()로 확인 애니메이션만 트리거한다(서버 반영은
// MicroSwLoop가 크랭크로 솔레노이드를 실제로 열 때 RunAltarSuccess가 담당).
// 반대 순서(칩이 먼저 온 경우)는 CardChecking() 쪽에서 태그가 뒤늦게 확인될 때 처리한다.
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
      Serial.println("[IrSensor] Chip detected");
      ir_chip_pending = true; // 솔레노이드 개방은 MicroSwLoop가 담당
      ir_chip_confirm_animated = false; // 새 칩 세션 시작

      if (tag_active)
      {
        ir_chip_confirm_animated = true;
        OnTagChipConfirmed();
        // tag_active는 여기서 false로 되돌리지 않는다 - 태그는 여전히 리더에
        // 붙어있고, 되돌리면 다음 폴링 때 재태그로 오인해 애니메이션이 반복된다.
      }
      else
      {
        Serial.println("[IrSensor] No active tag yet - solenoid stays closed unless a tag catches up before the crank click");
      }
    }
  }
}

//****************************************** Micro Switch *****************************************
// 회전 메커니즘이 한바퀴 돌면 딸깍 눌림 (외부 10K 풀업 → 평소 HIGH, 눌리면 LOW).
// IR센서로 칩이 들어온 걸 이미 확인했을 때만(빈 회전 제외) 처리하되, 솔레노이드는
// "지금 이 순간에도" 태그가 리더에 붙어있을 때(tag_active, blink/activate 무관)만
// 실제로 연다 - 태그 없이 칩만 넣고 돌리거나, 태그+칩이 한 번 확인된 뒤 태그를
// 떼고서 돌리는 경우엔 열리지 않는다(태그가 이미 없으므로 tag_active가 false로
// 리셋돼있음 - RfidLoop 참고). taken_chip+1/tagger_name 서버 반영(RunAltarSuccess)도
// setting_state가 아니라 실제로 여기서 열리는 시점에만 확정한다. pending_success_sound가
// 서있으면(태그+IR이 이미 확인된 상태) 이 시점에 성공음(1,1)을 재생한다.
void MicroSwInit()
{
  pinMode(MICRO_SW_PIN, INPUT);
}

void MicroSwLoop()
{
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
      Serial.println("[MicroSw] Click detected");

      // setting 상태에서는 태그 확인이 애초에 불가능하므로(CardChecking의 tag_active는
      // game_state=="activate"일 때만 세워짐), 역할 태그 없이 그냥 IR센서가 칩을
      // 감지한 상태(ir_chip_pending)라면 마이크로스위치가 눌릴 때 바로 배출한다
      // (스태프가 세팅 중 넣어본 생명칩을 태그 없이 다시 꺼낼 수 있어야 함).
      bool setting_state = (String)(const char *)my["game_state"] == "setting";

      if (ir_chip_pending)
      {
        if (tag_active || setting_state)
        {
          Serial.println(setting_state
              ? "[MicroSw] Setting mode - chip present, solenoid open (no tag needed)"
              : "[MicroSw] Tag still present + chip confirmed -> solenoid open");
          SolenoidPulse();
          // 실제로 열렸을 때만 대기 상태를 소비한다 - 태그가 늦게 도착해서
          // 아직 확인 안 된 채로 클릭이 먼저 지나가도(빈 회전 아님, 그냥 아직
          // 대기 중) ir_chip_pending을 꺼버리면 나중에 태그가 와도 이미 늦어버림.
          ir_chip_pending = false;
          ir_chip_confirm_animated = false;

          // 서버 반영(taken_chip+1/tagger_name)은 여기, 크랭크가 실제로 돌아서
          // 솔레노이드가 열리는 순간에만 확정한다 - setting_state(태그 없이 칩만
          // 배출)는 실제 게임 진행이 아니므로 제외.
          if (tag_active)
          {
            RunAltarSuccess();
          }

          if (pending_success_sound)
          {
            Mp3PlayLargeFolder(1, 1); // 성공음 - 태그+IR+크랭크가 다 확인된 시점
            pending_success_sound = false;
          }
        }
        else
        {
          Serial.println("[MicroSw] Chip present but tag not currently on reader - keep waiting");
        }
      }
    }
  }
}
