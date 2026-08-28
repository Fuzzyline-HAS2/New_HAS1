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

#include "HAS1_tagmachine_sub.h"

void setup() {
    Serial.begin(115200);
    fromSubSerial.begin(9600, SERIAL_8N1, HWSERIAL_RX, HWSERIAL_TX);
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
        String dataSetUp = fromSubSerial.readStringUntil('\n'); //추가 시리얼의 값을 수신하여 String으로 저장
        // Serial.println("received:" + String(dataSetUp[0])); //기본 시리얼에 추가 시리얼 내용을 출력
        if(dataSetUp[0] == 'R'){ 
            while(rfid_init_complete == false){
                Serial.println("Beetle RFID Initializing...");
                RfidInit(); 
                delay(3000);
            }
        }
    }
    RfidLoopMain(); 
}
