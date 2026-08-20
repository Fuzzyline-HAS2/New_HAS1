void UpdateBrightness() {
    int serverBrightness = my["brightness"].as<int>();
    if (serverBrightness <= 0 || serverBrightness > 100) {
        ledBrightness = DEFAULT_BRIGHTNESS;
    } else {
        ledBrightness = map(serverBrightness, 1, 100, 1, 255);
    }
    for (int i = 0; i < NeopixelNum; ++i)
        pixels[i].setBrightness(ledBrightness);
    AllNeoOn(currentNeoColor);
}

void NeopixelInit()
{
  for (int i = 0; i < NeopixelNum; ++i) {
    pixels[i].begin();
    pixels[i].setBrightness(ledBrightness);
  }
  for (int i = 0; i < NeopixelNum; ++i)
    AllNeoOn(WHITE);
}

void NeoBlink(int neo, int neoColor, int cnt, int blinkTime){
    for(int i = 0; i < cnt; i++){
        for(int j = 0; j < NumPixels[neo]; j++)
            pixels[neo].setPixelColor(j, pixels[neo].Color(color[BLACK][0], color[BLACK][1], color[BLACK][2]));
        pixels[neo].show();
        delay(blinkTime);
        for(int j = 0; j < NumPixels[neo]; j++)
            pixels[neo].setPixelColor(j, pixels[neo].Color(color[neoColor][0], color[neoColor][1], color[neoColor][2]));
        pixels[neo].show();
        delay(blinkTime);
    }
}

void AllNeoOn(int neoColor){
    currentNeoColor = neoColor;
    for (int i = 0; i < NeopixelNum; ++i) {
        for (int j = 0; j < NumPixels[i]; j++)
            pixels[i].setPixelColor(j, pixels[i].Color(color[neoColor][0], color[neoColor][1], color[neoColor][2]));
        pixels[i].show();
    }
}

void LineNeoOn(int changeColr, int baseColor, int cnt){
  for(int i = 0; i < NumPixels[LINE]; i++)
    pixels[LINE].setPixelColor(i,pixels[LINE].Color(color[baseColor][0],color[baseColor][1],color[baseColor][2]));
  for(int i = 0; i < cnt; i++)
    pixels[LINE].setPixelColor(i,pixels[LINE].Color(color[changeColr][0],color[changeColr][1],color[changeColr][2]));
  pixels[LINE].show();
}