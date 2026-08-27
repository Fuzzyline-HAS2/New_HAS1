#include "HAS2_Wifi.h"

HAS2_Wifi has2wifi("/updatemac.php");

void setup(){
    Serial.begin(115200);
    has2wifi.Setup("iotglove");
}

void loop(){
    //has2wifi.Loop();
    has2wifi.Receive("G1P1");
    delay(3000);
}