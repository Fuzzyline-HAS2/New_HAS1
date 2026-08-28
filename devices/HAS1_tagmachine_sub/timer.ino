void TimerInit(){
    gameTimerId = GameTimer.setInterval(1500,GameTimerFunc);
    // GameTimer.disable(gameTimerId);
}

void GameTimerFunc(){
    // Serial.println("T1:"+tag1+"_T2:"+tag2+"_T3:"+tag3);
    // fromSubSerial.println("T1:"+tag1+"_T2:"+tag2+"_T3:"+tag3);
}