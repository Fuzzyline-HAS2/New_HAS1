// 3호점 아이템박스의 네오픽셀과 동일하다. 
void showNeoStable(Adafruit_NeoPixel &neo)
{
    neo.show();
}

void lightColor(Adafruit_NeoPixel &neo, int c[3])
{
    neo.fill(neo.Color(c[0], c[1], c[2]));
    showNeoStable(neo);
}

void NeopixelInit()
{
  for (int i = 0; i < NeopixelNum; ++i)
  {
    pixels[i].begin();
    pixels[i].setBrightness(LED_BRIGHTNESS);
  }
  for (int i = 0; i < NeopixelNum; ++i)
  {
    lightColor(pixels[i], color[WHITE]);
  }
}

/**
 * @brief 현재 엔코더 값을 네오픽셀의 빨간색으로 표시하는 함수
 */
void EncoderNeopixelOn()
{
    int neoColor = readEncoderValue() / 24;       // 0~23, 24~4 ... 24마다 네오픽셀의 밝기가 증가함을 저장하는 변수
    int neoNum = 23 - (readEncoderValue() % 24);  // 현재 빨간색으로 표시되어야 할 네오픽셀 번호를 저장하는 변수
    for(int i = 0; i < NumPixels[NEO_ENCODER]; i++)
        pixels[NEO_ENCODER].setPixelColor(i,pixels[NEO_ENCODER].Color(color[neoColor+7][0], color[neoColor+7][1], color[neoColor+7][2])); // ENCODERBLUE0부터 시작하기 위해 + 7 더해줌
     pixels[NEO_ENCODER].setPixelColor(neoNum,pixels[NEO_ENCODER].Color(color[RED][0],color[RED][1],color[RED][2]));
     pixels[NEO_ENCODER].show();
}
void NeoBlink(int neo, int neoColor, int cnt, int blinkTime)
{
  for(int i = 0; i < cnt; i++){                          //0.5*10=5초동안 점멸
    lightColor(pixels[neo], color[BLACK]); //전체 off
    delay(blinkTime);
    lightColor(pixels[neo], color[neoColor]); //전체 적색on
    delay(blinkTime);                   //전체 적색on
  }
}

void AllNeoOn(int neoColor){
  for (int i = 0; i < NeopixelNum; ++i)
    lightColor(pixels[i], color[neoColor]);
}