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

// OTA가 "새 버전이 있는지" 판단하는 기준값. TTGO가 UART로 "V" 명령을 보내면 이 값을
// "VER:<n>" 형태로 응답한다(beetle_ota.ino의 QueryBeetleVersion). GitHub Release에
// 올라간 version.txt와 비교되므로, 배포할 때마다(scripts/deploy.py) 이 숫자를 올려야
// TTGO가 업데이트를 인식한다.
#define FIRMWARE_VER 1
#include "HAS1_tagmachine_sub.h"

void setup() {
    Serial.begin(115200);
    // OTA 프레임(최대 OTA_MAX_PAYLOAD+7바이트)이 한꺼번에 몰려도 유실 없이 받도록 여유 있게 확보.
    fromSubSerial.setRxBufferSize(1024);
    // 9600은 태그 문자열 정도는 충분하지만 OTA로 수백 KB 펌웨어를 보내기엔 너무 느려(수십 분)
    // TTGO(HAS1_tagmachine_main)와 함께 상향했다 — 양쪽 보레이트는 반드시 같아야 한다.
    fromSubSerial.begin(115200, SERIAL_8N1, HWSERIAL_RX, HWSERIAL_TX);
    String dataSetUp = "";
    delay(2000);
    Serial.println("INIT");
    while(1){
        fromSubSerial.println("W");
        Serial.println("W");
        if(fromSubSerial.available() > 0){
            String dataSetUp = fromSubSerial.readStringUntil('\n'); //추가 시리얼의 값을 수신하여 String으로 저장
            // Serial.println("received:" + String(dataSetUp[0])); //기본 시리얼에 추가 시리얼 내용을 출력
            if(dataSetUp[0] == 'W'){
                while(rfid_init_complete == false){
                    Serial.println("Beetle RFID Initializing...");
                    RfidInit();
                    delay(3000);
                }
                break;
            }
        }
        delay(1000);
    }
    Serial.println("INIT FINISH");
    TimerInit();
}
    
void loop() {
    if(fromSubSerial.available() > 0){
        // OTA 프레임 프리앰블(0xA5)은 일반 텍스트 명령('W'/'R'/'V')의 첫 글자로는 나올 수
        // 없으므로 peek로 먼저 걸러 바이너리 세션으로 분기한다. readStringUntil('\n')으로
        // 읽으면 바이너리 데이터 안에 섞인 0x0A 바이트 때문에 프레임이 중간에 잘려나간다.
        if(fromSubSerial.peek() == OTA_SOF1){
            fromSubSerial.read();  // SOF1 소비
            HandleOtaSession();
        }
        else{
            String dataSetUp = fromSubSerial.readStringUntil('\n'); //추가 시리얼의 값을 수신하여 String으로 저장
            // Serial.println("received:" + String(dataSetUp[0])); //기본 시리얼에 추가 시리얼 내용을 출력
            if(dataSetUp[0] == 'R'){
                while(rfid_init_complete == false){
                    Serial.println("Beetle RFID Initializing...");
                    RfidInit();
                    delay(3000);
                }
            }
            else if(dataSetUp[0] == 'V'){  // TTGO가 OTA 전 버전 조회를 위해 보내는 명령
                fromSubSerial.println("VER:" + String(FIRMWARE_VER));
            }
        }
    }
    RfidLoopMain();
}
