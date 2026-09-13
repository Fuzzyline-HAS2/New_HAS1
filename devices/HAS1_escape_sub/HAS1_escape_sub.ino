/**
 * @file Done_Escape_Sub_code.ino
 * @author 김병준 (you@domain.com)
 * @brief  uart 시리얼 통신으로 메인 TTGO랑 주고 받는 코드
 * @version 1.0
 * @date 2022-11-29
 * @CDC ON BOOTH (when you select Disabled, the serial port is RX(20), TX(21), if you need to print on the Arduino monitor via USB, you need to select Enable)
 * @copyright Copyright (c) 2022
 *
 */

#include "escape_sub.h" 

void setup() 
{
    delay(3000);
    Serial.begin(115200);
    SPI.begin(PN532_SCK, PN532_MISO, PN532_MOSI, -1); // PN532 3개가 공유하는 하드웨어 SPI 버스를 커스텀 핀으로 초기화 (CS는 각 nfc[i]가 개별 관리)
    String dataSetUp = "";
    // Serial.println("INIT");
    // while(1){
    //     Serial.println("W");
    //     if(Serial.available() > 0){
    //         String dataSetUp = Serial.readStringUntil('\n'); //추가 시리얼의 값을 수신하여 String으로 저장
    //         // Serial.println("received:" + String(dataSetUp[0])); //기본 시리얼에 추가 시리얼 내용을 출력
    //         if(dataSetUp[0] == 'W'){
    //             //Serial.println("Beetle Connected");
    //             while(rfid_init_complete_cnt < 3){
    //                 Serial.println("Beetle RFID Initializing...");
    //                 RfidInit();
    //                 delay(3000);
    //             }
    //             Serial.println("Beetle RFID INIT SUCCESS");
    //             break;
    //         }
    //     }
    //     delay(1000);
    // }
    // Serial.println("INIT FINISH");
    for (int i = 0; i < 3; i++){
        Serial.println("W");
        if(Serial.available() > 0){
            String dataSetUp = Serial.readStringUntil('\n'); //추가 시리얼의 값을 수신하여 String으로 저장
            // Serial.println("received:" + String(dataSetUp[0])); //기본 시리얼에 추가 시리얼 내용을 출력
            if(dataSetUp[0] == 'W'){
                //Serial.println("Beetle Connected");
                while(rfid_init_complete_cnt < 3){
                    Serial.println("Beetle RFID Initializing...");
                    RfidInit();
                    delay(3000);
                }
                Serial.println("Beetle RFID INIT SUCCESS");
                goto INITFINISH;
            }
        }
        delay(500);
    }
    ESP.restart();
    INITFINISH:;
}
void loop() {
    if(Serial.available() > 0){
        String dataSetUp = Serial.readStringUntil('\n'); //추가 시리얼의 값을 수신하여 String으로 저장
        // Serial.println("received:" + String(dataSetUp[0])); //기본 시리얼에 추가 시리얼 내용을 출력
        if(dataSetUp[0] == 'R'){
            ESP.restart();
            // Serial.println("Beetle Reset");
            // while(rfid_init_complete_cnt < 3){
            //     Serial.println("Beetle RFID Initializing...");
            //     RfidInit();
            //     delay(3000);
            // }
            // Serial.println("Beetle RFID INIT SUCCESS");
        }
    }
    RfidLoopMain();
    if(serialSend == true)
    {
        Serial.println("T1:"+structTagData[0].tagData+"_T2:"+structTagData[1].tagData+"_T3:"+structTagData[2].tagData);
        serialSend = false;
    }
}
