/**
 * @file HAS1_revival_machine.ino
 * @author YuBin Kim
 * @brief
 * @version 0.1
 * @date 2022-11-24 ~ 2022-11-26
 *
 * @copyright Copyright (c) 2022
 */

#define FIRMWARE_VER 68
#define PARTITION_VER 5
#include "HAS1_revival_machine.h"

//************************************************ Core1 ********************************************************************
/**
 * @brief Temple Intialize
 */
void TempleInit()
{
  has2wifi.Setup("badland");
  has2wifi.Send((String)(const char *)my["device_name"], "esp_version", String(FIRMWARE_VER));
  CardUploadInit();
  TelnetInit(); // Telnet 서버 시작 (WiFi 연결 완료 후) — 이후 Serial.* 출력은 telnet.ino로 미러링됨
  LogMemoryStats("Wi-Fi connected");
  BleAdvertiserInit();
  ota.setLogStream(Serial);
  ota.setOnSuccess([]() {
    has2wifi.Send((String)(const char *)my["device_name"], "device_state", "setting");
  });
  ota.setOnSkip([]() {
    has2wifi.Send((String)(const char *)my["device_name"], "device_state", "setting");
  });
    ota.setPartitionUpdate(
        "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_revival_machine/partitions.bin",
        "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_revival_machine/partitions.sig",
        "https://github.com/Fuzzyline-HAS2/New_HAS1/releases/download/HAS1_revival_machine/partition_version.txt",
        PARTITION_VER
    );
  SensorInit();  // IoT Glove 사용 센서, 모듈 세팅
  TimerInit();   // 타이머 세팅
  
}

/**
 * @brief 아두이노 기본 문법 (전원이 켜지면 한번만 실행)
 */
void setup()
{
#if REVIVAL_RFID_DIAGNOSTICS
  // Keep the actuator off before any serial/PN532 initialization or wait.
  pinMode(SOLENOID_PIN, OUTPUT);
  digitalWrite(SOLENOID_PIN, LOW);
  Serial.begin(115200);
  RfidDiagnosticSetup();
#else
  delay(1000);
  Serial.begin(115200);
  LogMemoryStats("boot");
  TempleInit();
  DataChange();
#if REVIVAL_RFID_RUNTIME_TRACE
  RfidTraceStartup(FIRMWARE_VER, (const char *)my["game_state"], (const char *)my["device_state"]);
#endif
#endif
}

/**
 * @brief 아두이노 기본 문법 (전원이 켜져있는동안 Core1에서 계속 실행)
 */
void loop()
{
#if REVIVAL_RFID_DIAGNOSTICS
  RfidDiagnosticLoop();
#else
#if REVIVAL_RFID_RUNTIME_TRACE
  RfidTraceLoopBegin();
  uint32_t tracePhaseStarted = micros();
#endif
  TelnetRun(); // Telnet 클라이언트 접속/데이터 처리
#if REVIVAL_RFID_RUNTIME_TRACE
  RfidTraceLoopStage(RFID_TRACE_TELNET, tracePhaseStarted);
  tracePhaseStarted = micros();
#endif
  TimerRun();
#if REVIVAL_RFID_RUNTIME_TRACE
  RfidTraceLoopStage(RFID_TRACE_TIMER, tracePhaseStarted);
  tracePhaseStarted = micros();
#endif
  NeoFunc();
#if REVIVAL_RFID_RUNTIME_TRACE
  RfidTraceLoopStage(RFID_TRACE_NEO, tracePhaseStarted);
  tracePhaseStarted = micros();
#endif
  const bool maintenanceAtLoopStart = CardUploadBlocksGameplay();
  CardUploadLoop();
  if (maintenanceAtLoopStart || CardUploadBlocksGameplay())
  {
    // If removal completes now, defer gameplay to the next loop: no second scan.
    SolenoidOff();
  }
  else if (activate_bool)
  {
    ActivateFunc();
  }
  else
  {
    AdminCardPollReady();  // ready 등 태그 비활성 상태에서도 MMMM 관리자 카드는 열려야 함
  }
#if REVIVAL_RFID_RUNTIME_TRACE
  RfidTraceLoopStage(RFID_TRACE_GAME, tracePhaseStarted);
  RfidTraceLoopEnd((const char *)my["game_state"], (const char *)my["device_state"]);
#endif
#endif
  RfidFlushHealthLog();
}
