// lightColor: Adafruit_NeoPixel에 없는 메서드를 전역 헬퍼로 구현
//             픽셀 전체를 지정 색상(rgb 배열)으로 채우고 show() 호출
void lightColor(Adafruit_NeoPixel &neo, int c[3]) {
  neo.fill(neo.Color(c[0], c[1], c[2]));
  neo.show();
}

void UpdateBrightness() {
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

void NeopixelInit()
{
  for (int i = 0; i < NeopixelNum; ++i) {
    pixels[i].begin();
    pixels[i].setBrightness(ledBrightness);
  }
  for (int i = 0; i < NeopixelNum; ++i) {
    lightColor(pixels[i], color[WHITE]);
  }
}

void NeoBlink(int neo, int neoColor, int cnt, int blinkTime){
    for(int i = 0; i < cnt; i++){                          //0.5*10=5초동안 점멸
        lightColor(pixels[neo], color[BLACK]); //전체 off
        lightColor(pixels[neo+3], color[BLACK]);
        delay(blinkTime);            
        lightColor(pixels[neo], color[neoColor]); //전체 적색on
        lightColor(pixels[neo+3], color[neoColor]);
        delay(blinkTime);                   //전체 적색on
    }
}

void AllNeoOn(int neoColor){
    for (int i = 0; i < NeopixelNum; ++i)
        lightColor(pixels[i], color[neoColor]);
}

void LineNeoDown(int changeColr, int baseColor, int cnt){
  for(int i = 0; i < NumPixels[LINE]; i++){
    pixels[LINE].setPixelColor(i,pixels[LINE].Color(color[baseColor][0],color[baseColor][1],color[baseColor][2]));
    pixels[LINE_SUB].setPixelColor(i,pixels[LINE_SUB].Color(color[baseColor][0],color[baseColor][1],color[baseColor][2]));
  }
  for(int i = 0; i < cnt; i++){
    pixels[LINE].setPixelColor(i,pixels[LINE].Color(color[changeColr][0],color[changeColr][1],color[changeColr][2]));
    pixels[LINE_SUB].setPixelColor(i,pixels[LINE_SUB].Color(color[changeColr][0],color[changeColr][1],color[changeColr][2]));
  }
  pixels[LINE].show();
  pixels[LINE_SUB].show();
}
void LineNeoUp(int changeColr, int baseColor, int cnt){
  for(int i = 0; i < NumPixels[LINE]; i++){
    pixels[LINE].setPixelColor(i,pixels[LINE].Color(color[baseColor][0],color[baseColor][1],color[baseColor][2]));
    pixels[LINE_SUB].setPixelColor(i,pixels[LINE_SUB].Color(color[baseColor][0],color[baseColor][1],color[baseColor][2]));
  }
  for(int i = 16; i > 16 - cnt; i--){
    pixels[LINE].setPixelColor(i,pixels[LINE].Color(color[changeColr][0],color[changeColr][1],color[changeColr][2]));
    pixels[LINE_SUB].setPixelColor(i,pixels[LINE_SUB].Color(color[changeColr][0],color[changeColr][1],color[changeColr][2]));
  }
  pixels[LINE].show();
  pixels[LINE_SUB].show();
}
void RoundNeoEffect(int neocolor)
{
  pixels[ROUND].clear();
  pixels[ROUND_SUB].clear();
  pixels[ROUND].show();
  pixels[ROUND_SUB].show();

  for(int i = 0; i < NumPixels[ROUND]/2; i++)
  {
    pixels[ROUND].setPixelColor(i,pixels[ROUND].Color(color[neocolor][0],color[neocolor][1],color[neocolor][2]));
    pixels[ROUND].setPixelColor(NumPixels[ROUND]-i,pixels[ROUND].Color(color[neocolor][0],color[neocolor][1],color[neocolor][2]));
    pixels[ROUND_SUB].setPixelColor(i,pixels[ROUND_SUB].Color(color[neocolor][0],color[neocolor][1],color[neocolor][2]));
    pixels[ROUND_SUB].setPixelColor(NumPixels[ROUND]-i,pixels[ROUND_SUB].Color(color[neocolor][0],color[neocolor][1],color[neocolor][2]));
    pixels[ROUND].show();
    pixels[ROUND_SUB].show();
    // esp_task_wdt_reset();  // [WDT 비활성화]
    delay(100);
  }
}
void RoundNeoEffectDown(int neocolor)
{
  for(int i = NumPixels[ROUND]/2; i >= 0; i--)
  {
    pixels[ROUND].setPixelColor(i,pixels[ROUND].Color(color[neocolor][0],color[neocolor][1],color[neocolor][2]));
    pixels[ROUND].setPixelColor(NumPixels[ROUND]-i,pixels[ROUND].Color(color[neocolor][0],color[neocolor][1],color[neocolor][2]));
    pixels[ROUND_SUB].setPixelColor(i,pixels[ROUND_SUB].Color(color[neocolor][0],color[neocolor][1],color[neocolor][2]));
    pixels[ROUND_SUB].setPixelColor(NumPixels[ROUND]-i,pixels[ROUND_SUB].Color(color[neocolor][0],color[neocolor][1],color[neocolor][2]));
    pixels[ROUND].show();
    pixels[ROUND_SUB].show();
    // esp_task_wdt_reset();  // [WDT 비활성화]
    delay(100);
  }
}

void RoundNeoToggle(int neoColor, int toggle){
  if(toggle%2 == 1){
    lightColor(pixels[ROUND], color[neoColor]);
    lightColor(pixels[ROUND_SUB], color[neoColor]);
  }
  else{
    lightColor(pixels[ROUND], color[BLACK]);
    lightColor(pixels[ROUND_SUB], color[BLACK]);
  }
}
void LineNeoToggle(int neoColor, int toggle){
  if(toggle%2 == 1){
    lightColor(pixels[LINE], color[neoColor]);
    lightColor(pixels[LINE_SUB], color[neoColor]);
  }
  else{
    lightColor(pixels[LINE], color[BLACK]);
    lightColor(pixels[LINE_SUB], color[BLACK]);
  }
}

void RoundNeoUp(int changeColr, int baseColor, int cnt){
  for(int i = 0; i < 30; i++){
    pixels[ROUND].setPixelColor(i,pixels[ROUND].Color(color[baseColor][0],color[baseColor][1],color[baseColor][2]));
    pixels[ROUND_SUB].setPixelColor(i,pixels[ROUND_SUB].Color(color[baseColor][0],color[baseColor][1],color[baseColor][2]));
  }
  for(int i = 0; i < cnt; i++){
    pixels[ROUND].setPixelColor(i,pixels[ROUND].Color(color[changeColr][0],color[changeColr][1],color[changeColr][2]));
    pixels[ROUND_SUB].setPixelColor(i,pixels[ROUND_SUB].Color(color[changeColr][0],color[changeColr][1],color[changeColr][2]));
  }
  pixels[ROUND].show();
  pixels[ROUND_SUB].show();
}