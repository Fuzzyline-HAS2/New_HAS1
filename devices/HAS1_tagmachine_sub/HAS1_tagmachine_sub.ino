 ;/**
 * @file HAS1_tagmachine_sub.ino
 * @author 김병준 (you@domain.com)
 * @brief
 * @version 1.0
 * @date 2022-11-29
 *
 * @copyright Copyright (c) 2022
 *
 */

#define FIRMWARE_VER 3
#define PARTITION_VER 1

#include "HAS1_tagmachine_sub.h"

const uint32_t tagmachineFirmwareVersion = FIRMWARE_VER;
const uint32_t tagmachinePartitionVersion = PARTITION_VER;
uint32_t tagmachineBootId = 0;
HardwareSerial fromSubSerial(1);
SimpleTimer GameTimer;
int gameTimerId = 0;
void (*ptrCurrentMode)() = nullptr;
Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS1);
bool rfid_init_complete = false;

static RfidRecoveryPolicy rfidRecovery;
static bool linkAcknowledged = false;
static unsigned long lastHelloMs = 0;
static unsigned long lastHeartbeatMs = 0;
static unsigned long nextRfidPollMs = 0;
static char lastReportedRfidStatus = '\0';
static bool deferredRfidReinit = false;

#if ESP_IDF_VERSION_MAJOR >= 5
static esp_task_wdt_user_handle_t loopWatchdog = nullptr;
#endif
static bool watchdogEnabled = false;

static void SendRfidStatus(char status, bool force = false) {
    if (!force && lastReportedRfidStatus == status) return;
    fromSubSerial.println(status);
    lastReportedRfidStatus = status;
}

static void SendHeartbeat(void) {
    // H는 MCU 생존 신호이고, 바로 뒤의 A/E가 PN532 상태다. 모두 한 글자라 구형
    // TTGO의 "4글자 이상=태그" 규칙에도 태그로 오인되지 않는다.
    fromSubSerial.println("H");
    SendRfidStatus(rfid_init_complete ? 'A' : 'E', true);
}

static void WatchdogInit(void) {
#if ESP_IDF_VERSION_MAJOR >= 5
    const esp_task_wdt_config_t config = {
        .timeout_ms = BEETLE_WATCHDOG_TIMEOUT_MS,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    esp_err_t result = esp_task_wdt_reconfigure(&config);
    if (result == ESP_ERR_INVALID_STATE) result = esp_task_wdt_init(&config);
    if (result == ESP_OK) result = esp_task_wdt_add_user("tagmachine-loop", &loopWatchdog);
    watchdogEnabled = (result == ESP_OK);
#else
    esp_err_t result = esp_task_wdt_init(BEETLE_WATCHDOG_TIMEOUT_MS / 1000, true);
    if (result == ESP_OK || result == ESP_ERR_INVALID_STATE) {
        result = esp_task_wdt_add(NULL);
    }
    watchdogEnabled = (result == ESP_OK);
#endif
    Serial.println(watchdogEnabled ? "[WDT] 12s watchdog started"
                                   : "[WDT] watchdog unavailable");
}

static void WatchdogFeed(void) {
    if (!watchdogEnabled) return;
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_reset_user(loopWatchdog);
#else
    esp_task_wdt_reset();
#endif
}

void RequestRfidReinit(bool recoveryAckRequired) {
    const unsigned long now = millis();
    rfid_init_complete = false;
    rfidRecovery.request(now, recoveryAckRequired);
    // deadlineReached(now, 0)는 uptime이 2^31ms를 넘으면 false다. 현재 시각을
    // 명시해야 장기간 가동 후 재초기화에서도 polling이 즉시 재개된다.
    nextRfidPollMs = now;
}

static void ServiceRfidInitialization(unsigned long now) {
    if (!rfidRecovery.attemptDue(now)) return;

    Serial.println("Beetle RFID Initializing...");
    if (!RfidInit()) {
        rfidRecovery.recordFailure(millis());
        SendRfidStatus('E');
        Serial.print("PN532 retry in ms: ");
        Serial.println(RfidRecoveryPolicy::retryDelayMs(rfidRecovery.failureCount()));
        return;
    }

    rfidRecovery.recordSuccess();
    nextRfidPollMs = millis();
    // 구형 TTGO도 R을 이미 reset-success 제어문자로 취급한다. 요청에 대한 완료 ACK를
    // 먼저 보내야 뒤따르는 A가 구형 수신기의 flush에 지워져도 R은 보인다.
    const bool recoveryAcknowledged = rfidRecovery.takeRecoveryAck();
    if (recoveryAcknowledged) fromSubSerial.println("R");
    SendRfidStatus('A', recoveryAcknowledged);
    Serial.println("INIT FINISH");
}

static void HandleControlFrame(const char *frame, size_t length) {
    if (length != 1) return;

    switch (frame[0]) {
        case 'W':
            // Beetle이 보낸 boot hello에 대한 ACK. 다시 W를 echo하면 양쪽이 무한히
            // 응답할 수 있으므로 여기서는 연결 상태만 기록한다.
            linkAcknowledged = true;
            Serial.println("TTGO link acknowledged");
            break;
        case 'H':
            // TTGO가 능동 probe를 보낸 경우 현재 MCU/PN532 상태를 즉시 돌려준다.
            SendHeartbeat();
            break;
        case 'R':
            if (OtaBusy()) {
                deferredRfidReinit = true;
                Serial.println("TTGO PN532 reinitialization deferred until OTA completes");
            } else {
                Serial.println("TTGO requested PN532 reinitialization");
                RequestRfidReinit(true);
            }
            break;
        default:
            break;
    }
}

static void ServiceSerial(void) {
    static char frame[tagmachine::ota_wire::kMaxLine + 1];
    static size_t length = 0;
    static bool droppingOversizedFrame = false;

    while (fromSubSerial.available() > 0) {
        const char value = static_cast<char>(fromSubSerial.read());
        if (value == '\r') continue;
        if (value == '\n') {
            if (!droppingOversizedFrame && length > 0) {
                frame[length] = '\0';
                tagmachine::ota_wire::Command command;
                if (tagmachine::ota_wire::parseCommand(frame, length, command)) {
                    OtaHandleCommand(command);
                } else if (!(length >= 2 &&
                             (frame[0] == 'Q' || frame[0] == 'U') &&
                             frame[1] == ':')) {
                    HandleControlFrame(frame, length);
                }
            }
            length = 0;
            droppingOversizedFrame = false;
            continue;
        }
        if (droppingOversizedFrame) continue;
        if (length < sizeof(frame) - 1) {
            frame[length++] = value;
        } else {
            // 손상되거나 예상보다 긴 프레임은 다음 newline까지 버린다.
            length = 0;
            droppingOversizedFrame = true;
        }
    }
}

static void ServiceLink(unsigned long now) {
    if (!linkAcknowledged && (lastHelloMs == 0 || now - lastHelloMs >= BEETLE_HELLO_INTERVAL_MS)) {
        fromSubSerial.println("W");
        lastHelloMs = now;
    }

    if (lastHeartbeatMs == 0 || now - lastHeartbeatMs >= BEETLE_HEARTBEAT_INTERVAL_MS) {
        lastHeartbeatMs = now;
        SendHeartbeat();
    }
}

void setup() {
    Serial.begin(115200);
    fromSubSerial.begin(9600, SERIAL_8N1, HWSERIAL_RX, HWSERIAL_TX);
    while (tagmachineBootId == 0) tagmachineBootId = esp_random();
    delay(100);
    Serial.println("INIT");
    TimerInit();
    WatchdogInit();
    OtaInit();

    // TTGO 응답과 PN532 초기화를 서로 종속시키지 않는다. 두 서비스 모두 loop에서
    // 진행되므로 한쪽이 늦게 켜지거나 실패해도 UART 복구 명령은 계속 처리할 수 있다.
    fromSubSerial.println("W");
    lastHelloMs = millis();
    RequestRfidReinit(false);
}

void loop() {
    ServiceSerial();
    OtaPoll();
    const unsigned long now = millis();
    if (!OtaBusy() && deferredRfidReinit) {
        deferredRfidReinit = false;
        RequestRfidReinit(true);
    }
    if (!OtaBusy()) ServiceRfidInitialization(now);
    // 초기화가 즉시 성공하면 첫 heartbeat부터 A를 보내고, 실패한 경우에만 E를 보낸다.
    ServiceLink(now);

    if (!OtaBusy() && rfid_init_complete &&
        RfidRecoveryPolicy::deadlineReached(now, nextRfidPollMs)) {
        nextRfidPollMs = now + RFID_POLL_INTERVAL_MS;
        RfidLoopMain();
    }

    if (OtaHealthy(now)) WatchdogFeed();
    delay(2); // idle task와 UART driver가 실행될 기회를 보장한다.
}
