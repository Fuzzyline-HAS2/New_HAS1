#include "HAS1_duct.h"

//****************************************** Initialize ******************************************
void SensorInit()
{
  // Relay Init
  pinMode(RELAY_PIN, OUTPUT);

  // Push Button Init
  pinMode(SW_PIN, INPUT_PULLUP);

  // EMCHECK_PIN Init
  pinMode(EMCHECK_PIN, INPUT_PULLUP);

  // Emergency Init - interrupt
  pinMode(EMNERGENCY_CHK_PIN, INPUT_PULLUP);

  // Neopixel init
  pixels_line.begin();
  pixels_round.begin();
  pixels_switch.begin();
  pixels_line.setBrightness(lineBrightness);
  pixels_round.setBrightness(colorBrightness);
  pixels_switch.setBrightness(colorBrightness);

  // Rfid init
  RfidInit();

  // DFRobot MP3
  MySerial2.begin(9600, SERIAL_8N1, SERIAL2_RX_PIN, SERIAL2_TX_PIN);
  Mp3Init();
}

//********************************************* Rfid *********************************************
/**
 * @brief RFID(=PN532) 세팅
 */
void RfidInit()
{
  nfc.begin();
  if (!(nfc.getFirmwareVersion()))
  {
    Serial.print("!!!RFID 연결실패!!!");
    if (!send_rfid_error)
    {
      has2wifi.Send((String)(const char *)my["device_name"], "device_state", "PN532");
      send_rfid_error = true;
    }
    return;
  }
  nfc.SAMConfig();
  Serial.println("RFID 연결성공");
}

/**
 * @brief RFID 태그 인식
 */
void RfidLoop()
{
  // "이로운 효과" 동결 중에도 Duct Kill을 위해 RFID는 계속 읽는다.
  // (일반 덕트 사용 차단은 CardChecking에서 tagger_mode로 게이팅)
  if (!rfid_tag)
  {
    rfid_tag = true;
    rfid_timer_id = rfid_timer.setTimeout(500, RfidTagTimerFunc);
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
  if (nfc.sendCommandCheckAck(pn532_packetbuffer11, 1))
  { // rfid 통신 가능한 상태인지 확인
    if (nfc.startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A))
    {                                    // rfid에 tag 찍혔는지 확인용 //데이터 들어오면 uid정보 가져오기
      if (nfc.ntag2xx_ReadPage(7, data)) // ntag 데이터에 접근해서 불러와서 data행열에 저장
        CardChecking(data);
    }
  }
}

/**
 * @brief RFID에 태그된 NFC의 데이터에 따른 코드 동작
 *
 * @param rfidData 태그된 NFC의 데이터
 */
void CardChecking(uint8_t rfidData[32]) // 어떤 카드가 들어왔는지 확인용
{
  String tagUser = "";

  for (int i = 0; i < 4; i++)
  { // GxPx 데이터만 배열에서 추출해서 string으로 저장
    tagUser += (char)rfidData[i];
  }
  Serial.println("tag_user_data : " + tagUser);

  // MMMM 관리자 카드: game_state/tagger_mode(봉쇄) 상태와 무관하게 최우선으로 항상 연다.
  if (tagUser == "MMMM")
  {
    MmmmOpen();
    return;
  }

  if (game_state == activate)
  {
    // 1. 태그한 플레이어의 역할과 생명칩갯수, 최대생명칩갯수 등 읽어오기
    has2wifi.Receive(tagUser);
    // 2. 술래인지, 플레이어인지 구분
    if ((String)(const char *)tag["role"] == "player" || (String)(const char *)tag["role"] == "revival" || (String)(const char *)tag["role"] == "ghost")
    {
      if (tagger_mode) TaggerModeTagBlocked();   // 봉쇄(덕트킬 포함) 중 태그 → 보라색 점멸 + 사용불가 안내
      else DuctTag(tagUser);
    }
    else if ((String)(const char *)tag["role"] == "tagger" && ((int)tag["taken_chip"] < (int)tag["max_taken_chip"]) && (String)(const char *)tag["device_state"] == "activate")
    {
      if (digitalRead(EMCHECK_PIN) && !tagger_mode)
      {
        DuctKill();
      }
    }
  }
  else if (game_state == setting)
  {
    if (!tagger_mode)   // "이로운 효과" 동결 중 스태프 도어도 차단
    {
      Serial.println("staff");
      digitalWrite(RELAY_PIN, HIGH);
      delay(5000);
      digitalWrite(RELAY_PIN, LOW);
    }
  }
}

//********************************************* Mp3 **********************************************
/**
 * @brief DF Player Mini 세팅
 */
void Mp3Init()
{
  Serial.println();
  Serial.println(F("DFRobot DFPlayer Mini Demo"));
  Serial.println(F("Initializing DFPlayer ... (May take 3~5 seconds)"));

  myDFPlayer.setTimeOut(1000); // Set serial communictaion time out 1000 ms

  if (!myDFPlayer.begin(MySerial2))
  { // Use softwareSerial to communicate with mp3.
    Serial.println(F("Unable to begin:"));
    Serial.println(F("1.Please recheck the connection!"));
    Serial.println(F("2.Please insert the SD card!"));
    return;
  }
  Serial.println(F("DFPlayer Mini online."));
  mp3_available = true;
  myDFPlayer.setTimeOut(500); // Set serial communictaion time out 500ms
  //----Set volume----
  myDFPlayer.volume(30); // Set volume value (0~30).
  //----Set different EQ----
  myDFPlayer.EQ(DFPLAYER_EQ_NORMAL);
  // myDFPlayer.enableDAC();  //Enable On-chip DAC
  myDFPlayer.outputDevice(DFPLAYER_DEVICE_SD);
}

/**
 * @brief DF Player Mini MP3 플레이
 *
 * @param folder_number 큰 폴더 이름 [01, 02 등]
 * @param file_number   폴더 내 MP3 파일이름 [0001 ~, 0002 ~ 등]
 */
void Mp3PlayLargeFolder(uint8_t folder_number, uint16_t file_number)
{
  if (!mp3_available) return;

  static uint8_t play_error_count = 0; // MP3 파일이 처음 실행되면

  uint8_t add_folder_number = 4; // 영어버전 시 mp3 폴더 변경

  if ((String)(const char *)shift_machine["selected_language"] == "EN")
  {
    folder_number = add_folder_number + folder_number;
    myDFPlayer.volume(26);
  }
  else
  {
    myDFPlayer.volume(30);
  }

  if (myDFPlayer.available())
  {
    myDFPlayer.playLargeFolder(folder_number, file_number);
    play_error_count = 0;
  }
  else
  {
    if (play_error_count < 3)
    {
      myDFPlayer.playLargeFolder(folder_number, file_number);
      play_error_count++;
      Serial.print("에러횟수 :");
      Serial.println(play_error_count);
    }
    else
    {
      Serial.println("MP3 play error: max retries exceeded");
    }
  }
}

void Mp3Check()
{
  if (mp3_open)
  {
    mp3_open = false;
    Mp3PlayLargeFolder(1, 2);
  }
  if (mp3_cool)
  {
    mp3_cool = false;
    CooltimeMp3();
  }
}

void CooltimeMp3()
{
  int cooltime_min = (cooltime - current_time) / 60;
  int cooltime_sec = (cooltime - current_time) % 60;
  Serial.print("min : ");
  Serial.println(cooltime_min);
  Serial.print("sec : ");
  Serial.println(cooltime_sec);
  Mp3PlayLargeFolder(1, 3);
  delay(2800);
  if (cooltime_min > 0)
  {
    Mp3PlayLargeFolder(2, cooltime_min);
    delay(1100);
    Mp3PlayLargeFolder(1, 4);
    delay(500);
  }
  if (cooltime_min <= 0)
  {
    Mp3PlayLargeFolder(3, cooltime_sec);
    delay(1300);
    Mp3PlayLargeFolder(1, 5);
    delay(500);
  }
}

//******************************************* Switch ********************************************
/**
 * @brief 비상탈출 스위치를 누르면 인터럽트로 동작하는 함수
 */
void EmegencyPush()
{
  // Todo main으로 보내는 코드 추가
  if (!digitalRead(EMNERGENCY_CHK_PIN) && ((String)(const char *)my["device_state"] != "emergency"))
  {
    has2wifi.Send((String)(const char *)my["device_name"], "device_state", "emergency");
  }
}
