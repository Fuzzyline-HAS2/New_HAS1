void TagCount(){
    tagCnt = 0;
    for(int  i = 0; i < 3; i++){
        if(tagState[i] == true)
            tagCnt++;
    }
    if(tagCnt == 1){
        Serial.println("Tag 1 detected");
        Mp3PlayLargeFolder(1, VE2);
    }
    else if(tagCnt == 2){
        Serial.println("Tag 2 detected");
        Mp3PlayLargeFolder(1, VE3);
    }
    else if(tagCnt >= 3){
        Serial.println("Escape Activate");
        Mp3PlayLargeFolder(1, VE4);
        SendDeviceStateWithRetry("escape");
        EscapeClose();
        GameTimer.disable(gameTimerId);
        ptrCurrentMode = WaitFunc;
        delay(3000);
        Mp3PlayLargeFolder(1, VE5);
    }

    for(int  i = 0; i < 3; i++)
        tagState[i] = false;
}