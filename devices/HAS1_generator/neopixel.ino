// =================================================================================
// neopixel.ino
// ---------------------------------------------------------------------------------
// 4개의 네오픽셀(WS2812) 스트립(GAUGE/STARTER/DEVICESTATE/CIRCUIT)을 제어하는 함수 모음.
// 다른 탭(Game_system.ino, wifi.ino, rfid.ino, timer.ino, wire.ino)에서 상태가 바뀔 때마다
// 이 파일의 함수를 호출해 LED로 현재 상태를 시각적으로 표시한다.
// =================================================================================

// 지정한 스트립(neoIdx) 전체를 rgb 배열의 단색으로 채우고 즉시 출력한다.
void NeoLightColor(int neoIdx, int* rgb) {
  pixels[neoIdx].fill(pixels[neoIdx].Color(rgb[0], rgb[1], rgb[2]));
  pixels[neoIdx].show();
}

// setup()에서 1회 호출: 4개 스트립 모두 초기화하고 밝기를 적용한 뒤 흰색으로 점등한다.
void NeopixelInit()
{
  for (int i = 0; i < NeopixelNum; ++i)
  {
    pixels[i].begin();
    pixels[i].setBrightness(ledBrightness);
  }
  for (int i = 0; i < NeopixelNum; ++i)
  {
    NeoLightColor(i, color[WHITE]);
  }
}

// 서버(my["brightness"], 1~100)에서 받은 밝기 값을 네오픽셀 밝기 범위(1~255)로 변환해 4개 스트립에 즉시 적용한다.
// 범위를 벗어난 값(<=0 또는 >100)이 오면 DEFAULT_BRIGHTNESS로 안전하게 대체한다.
void UpdateBrightness()
{
  int serverBrightness = my["brightness"].as<int>();
  if (serverBrightness <= 0 || serverBrightness > 100) {
    ledBrightness = DEFAULT_BRIGHTNESS;
  } else {
    ledBrightness = map(serverBrightness, 1, 100, 1, 255);
  }
  for (int i = 0; i < NeopixelNum; ++i) {
    pixels[i].setBrightness(ledBrightness);
    pixels[i].show();
  }
}

// 배선(0~max) 개수를 GAUGE(28개)에 비례 채움 — StarterActivate 단계 전, 배터리팩 충전 단계 전용
// cnt/maxCnt 비율만큼 앞에서부터 파란색으로 채우고, 나머지는 꺼진 상태(검정)로 만든다.
void BatteryGaugeShow(int cnt, int maxCnt){
  int litPixels = (maxCnt > 0) ? (int)((long)NumPixels[GAUGE] * cnt / maxCnt) : 0;
  for(int i = 0; i < litPixels; i++)
    pixels[GAUGE].setPixelColor(i, pixels[GAUGE].Color(color[BLUE][0], color[BLUE][1], color[BLUE][2]));
  for(int i = litPixels; i < NumPixels[GAUGE]; i++)
    pixels[GAUGE].setPixelColor(i, pixels[GAUGE].Color(color[BLACK][0], color[BLACK][1], color[BLACK][2]));
  pixels[GAUGE].show();
}

// 스타터(엔코더) 진행 단계에서 GAUGE 스트립을 갱신한다.
// neoNum번째 픽셀까지는 파란색(진행 완료 구간), 그 뒤는 초록색(남은 구간)으로 표시한다.
// BatteryGaugeShow와 달리 "꺼짐" 대신 초록색을 써서 스타터 단계임을 구분한다.
void EncoderNeopixelOn(int neoNum){
  for(int i = 0; i < neoNum; i++)
    pixels[GAUGE].setPixelColor(i,pixels[GAUGE].Color(color[BLUE][0], color[BLUE][1], color[BLUE][2]));
  for(int i = neoNum; i < NumPixels[GAUGE]; i++)
    pixels[GAUGE].setPixelColor(i,pixels[GAUGE].Color(color[GREEN][0],color[GREEN][1],color[GREEN][2]));
  pixels[GAUGE].show();
}

// 지정한 스트립(neo)을 neoColor 색상으로 cnt번 점멸시킨다 (blocking: delay 사용, 그동안 loop가 멈춤).
// BlinkTimer를 쓰는 비동기 점멸(BlinkTimerFunc)과 달리, 이 함수는 호출 즉시 동기적으로 실행된다.
void NeoBlink(int neo, int neoColor, int cnt, int blinkTime){
  for(int i = 0; i < cnt; i++){                          //0.5*10=5초동안 점멸
    NeoLightColor(neo, color[BLACK]); //전체 off
    delay(blinkTime);
    NeoLightColor(neo, color[neoColor]); //전체 적색on
    delay(blinkTime);                   //전체 적색on
  }
}

// 4개 스트립 전체를 동일한 색(neoColor)으로 켠다. 게임 상태 전환 시(예: 대기/준비/활성화) 자주 사용된다.
void AllNeoOn(int neoColor){
  for (int i = 0; i < NeopixelNum; ++i)
    NeoLightColor(i, color[neoColor]);
}
