#!/usr/bin/env python3
"""Compile real starter/tagger rendering and decay functions with fake hardware."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def function(file, name):
    source = (ROOT / file).read_text()
    start = source.index('void ' + name + '(')
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

prefix = r'''
#include <cassert>
#include <cstdint>
#include <iostream>
unsigned long now = 0;
unsigned long millis() { return now; }
enum { GAUGE, STARTER, DEVICESTATE, CIRCUIT };
enum { WHITE, RED, YELLOW, GREEN, BLUE, PURPLE, BLACK };
int NumPixels[] = {28,12,16,10};
int color[7][3] = {{255,255,255},{255,0,0},{255,255,0},{0,255,0},{0,0,255},{255,0,255},{0,0,0}};
struct Pixel {
    uint32_t values[28] = {};
    uint32_t Color(int r,int g,int b) { return (r<<16)|(g<<8)|b; }
    void setPixelColor(int i,uint32_t c) { assert(i>=0 && i<28); values[i]=c; }
    void show() {}
} pixels[4];
bool starterTaggerActive=false, starterRfidNeedsValidation=false;
bool taggerPurpleVisible=true, taggerLastTagState=true;
bool attached=true, audioDone=false;
int lockResets=0, audioStarts=0;
int displayedGaugeNeoCnt=10, starterContribLastCnt=0;
long encoderValue=40000;
int starterEncoderUnit=4000, starterDecreaseAmount=1125;
unsigned int gameTimerCnt=0;
void WaitFunc() {}
void StarterActivate() {}
void TaggerRfidLoop() {}
void (*ptrCurrentMode)()=StarterActivate;
void (*ptrRfidMode)()=WaitFunc;
struct Timer { void deleteTimer(int) {} } BlinkTimer;
int blinkTimerId=0;
void EncoderDetach() { attached=false; }
void RfidPresenceReset() { ++lockResets; }
void Mp3TaggerStart() { ++audioStarts; }
bool Mp3TaggerFinished() { return audioDone; }
int StarterGaugeCnt() { int v=encoderValue/starterEncoderUnit; return v<0 ? 0 : (v>28 ? 28 : v); }
void AllNeoOn(int c) { for(auto& p:pixels) for(int i=0;i<28;++i) p.setPixelColor(i,p.Color(color[c][0],color[c][1],color[c][2])); }
void StarterGaugeUpdate(bool);
'''
wifi_stubs = r'''
#include <string>
#include <map>
struct String : std::string {
    using std::string::string;
    String(int n):std::string(std::to_string(n)) {}
};
struct Value {
    std::string text;
    Value& operator=(const char* p) { text=p; return *this; }
    Value& operator=(int n) { text=std::to_string(n); return *this; }
    operator const char*() const { return text.c_str(); }
    operator int() const { return text.empty()?0:std::stoi(text); }
    template<class T> bool is() { return true; }
    template<class T> T as() { return text.c_str(); }
};
struct Document {
    std::map<std::string,Value> values;
    Value& operator[](const char* k) { return values[k]; }
    bool containsKey(const char* k) { return values.count(k); }
} my, cur;
#define BREADCRUMB(x) ((void)0)
namespace Has1BleBeacon { void setDeviceName(const char*) {} }
struct Wifi { void Send(String,const char*,String) {} } has2wifi;
struct Log { template<class T> void println(T) {} } Serial;
struct Ota { void check() {} } ota;
void esp_task_wdt_delete(void*) {}
void esp_task_wdt_add(void*) {}
Timer GameTimer;
int gameTimerId=0;
bool receiveMineOn=false;
int batteryRenders=0, activations=0;
void UpdateBrightness() {}
void SettingFunc() { TaggerReset(); ptrCurrentMode=WaitFunc; }
void ReadyFunc() { TaggerReset(); ptrCurrentMode=WaitFunc; }
void ActivateFunc() { ++activations; }
void LeftGenerator() {}
void BatteryPackSend() { ++batteryRenders; }
void Mp3PlayLargeFolder(int,int) {}
void Mp3PlayLargeFolderAndWait(int,int) {}
void BatteryFinish() {}
void WirePollMain() {}
void WireResetTracking() {}
void WireObserveServerSnapshot() {}
void NeoLightColor(int,int*) {}
'''
tests = r'''
void checkGauge(int blue, uint32_t background) {
    for(int i=0;i<28;++i) assert(pixels[GAUGE].values[i] == (i<blue ? 0x0000ffu : background));
}
int main() {
    AllNeoOn(BLUE);
    encoderValue=60000; // pending charge animation exceeds the 10 visible pixels
    TaggerEnter();
    assert(starterTaggerActive && !attached && ptrCurrentMode==TaggerRfidLoop);
    assert(starterRfidNeedsValidation && !taggerLastTagState && lockResets==1);
    checkGauge(10,0xff00ff);
    for(int s=1;s<4;++s) for(int i=0;i<NumPixels[s];++i) assert(pixels[s].values[i]==0xff);
    now=1000; StarterGaugeUpdate(false); checkGauge(10,0xff00ff);
    encoderValue=40000;
    for(int i=0;i<5;++i) GameTimerFunc(); // existing initial decay threshold
    assert(encoderValue==38875 && gameTimerCnt==3);
    now+=100; StarterGaugeUpdate(false); checkGauge(9,0xff00ff);
    GameTimerFunc(); GameTimerFunc(); // subsequent threshold remains 800ms
    assert(encoderValue==37750);
    TaggerFeedbackStart(); assert(audioStarts==1 && TaggerFeedbackBusy());
    TaggerFeedbackLoop(); checkGauge(9,0xff00ff);
    audioDone=true; TaggerFeedbackLoop(); checkGauge(9,0);
    encoderValue=32000; now+=400; TaggerFeedbackLoop(); checkGauge(8,0xff00ff);
    now+=400; TaggerFeedbackLoop(); checkGauge(8,0);
    now+=400; TaggerFeedbackLoop(); checkGauge(8,0xff00ff);
    now+=400; TaggerFeedbackLoop(); assert(!TaggerFeedbackBusy());
    TaggerReset(); ptrCurrentMode=StarterActivate; StarterGaugeUpdate(true);
    checkGauge(8,0x00ff00); assert(encoderValue==32000 && !attached);
    // Interrupted feedback must not leak into the next tagger activation.
    TaggerEnter(); TaggerFeedbackStart(); TaggerReset();
    assert(!TaggerFeedbackBusy() && taggerPurpleVisible && !taggerLastTagState);
    ptrCurrentMode=WaitFunc; TaggerEnter(); assert(!starterTaggerActive);
    for(auto& p:pixels) for(int i=0;i<28;++i) assert(p.values[i]==0xff00ff);
    ptrCurrentMode=StarterActivate; encoderValue=0; displayedGaugeNeoCnt=0;
    TaggerEnter(); now+=100; GameTimerFunc(); StarterGaugeUpdate(false);
    checkGauge(0,0xff00ff); assert(encoderValue>=0);
    // Real DataChanged dispatch: simultaneous battery change cannot swallow tagger.
    my["game_state"]="activate"; my["device_state"]="tagger";
    my["battery_pack"]=4; my["max_battery_pack"]=4;
    cur=my; cur["device_state"]="battery_max"; cur["battery_pack"]=3;
    ptrCurrentMode=StarterActivate; receiveMineOn=true;
    encoderValue=32000; displayedGaugeNeoCnt=8;
    DataChanged();
    assert(starterTaggerActive && ptrCurrentMode==TaggerRfidLoop && !receiveMineOn);
    assert(batteryRenders==0);
    my["device_state"]="battery_max"; my["battery_pack"]=2;
    DataChanged();
    assert(!starterTaggerActive && ptrCurrentMode==StarterActivate);
    assert(encoderValue==32000 && activations==0 && batteryRenders==0);
    checkGauge(8,0x00ff00);
    ptrCurrentMode=WaitFunc; my["device_state"]="tagger"; DataChanged();
    my["device_state"]="starter_finish"; DataChanged();
    assert(activations==1);
    std::cout << "PASS: starter overlay, frozen charge animation, decay, purple-only feedback, resume, reset, non-starter, zero gauge, server transitions\n";
}
'''
source = prefix + function('neopixel.ino','EncoderNeopixelOn') + '\n' + function('Game_system.ino','StarterGaugeUpdate') + '\n' + function('timer.ino','GameTimerFunc') + '\n' + (ROOT/'tagger.ino').read_text() + wifi_stubs + function('wifi.ino','DataChanged') + tests
with tempfile.TemporaryDirectory(prefix='generator-tagger-') as directory:
    cpp = Path(directory)/'test.cpp'
    binary = Path(directory)/'test'
    cpp.write_text(source)
    subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
