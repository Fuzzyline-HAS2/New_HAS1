#include "HAS2_Wifi.h"

HAS2_Wifi has2wifi;

void setup()
{
    Serial.begin(115200);
    has2wifi.Setup("iotglove");
}

void loop()
{
    has2wifi.Loop();
    Serial.print("DeviceName : "); Serial.println((const char*)my["DeviceName"]);
    Serial.print("LifeChip : "); Serial.println((int)my["LifeChip"]);
    Serial.println();
    delay(2000);
}