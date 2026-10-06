#!/usr/bin/env python3
"""Compile the production wire state/transport/parser against a fake device/server."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
include = os.environ.get("ARDUINOJSON_INCLUDE")
if not include:
    for candidate in (Path("/private/tmp/generator-absolute-deps/ArduinoJson/src"),
                      ROOT.parents[1]/".testdeps/ArduinoJson/src"):
        if (candidate/"ArduinoJson.h").is_file():
            include=str(candidate)
            break
if not include:
    raise SystemExit("Set ARDUINOJSON_INCLUDE to ArduinoJson v7.4.3/src (see tests/README.md).")

PREFIX = r'''
#include <ArduinoJson.h>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "generator_wire_state.h"
#include "generator_wire_protocol.h"
static uint32_t now = 0;
static int criticalDepth = 0;
static bool pauseSampler = false, failSamplerCreation = false;
static uint32_t nextSamplerWake = 0;
static void (*fakeTaskEntry)(void*) = nullptr;
struct StaticTask_t {};
using StackType_t = uint8_t;
using TaskHandle_t = void*;
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) do { (void)(x); assert(++criticalDepth==1); } while(0)
#define portEXIT_CRITICAL(x) do { (void)(x); assert(--criticalDepth==0); } while(0)
#define pdMS_TO_TICKS(x) (x)
#define tskNO_AFFINITY -1
struct TaskYield {};
void vTaskDelay(uint32_t ticks) { assert(criticalDepth==0 && ticks==100); nextSamplerWake=now+ticks; throw TaskYield(); }
TaskHandle_t xTaskCreateStaticPinnedToCore(void (*entry)(void*),const char*,size_t,void*,int,StackType_t*,StaticTask_t*,int) {
 assert(criticalDepth==0);
 if(failSamplerCreation) return nullptr;
 fakeTaskEntry=entry; nextSamplerWake=now; return reinterpret_cast<void*>(1);
}
void advanceTime(uint32_t elapsed) {
 const uint32_t target=now+elapsed;
 if(fakeTaskEntry && !pauseSampler) {
   if(nextSamplerWake<now) nextSamplerWake=now; // Actual observation now; no back-filled ticks.
   while(nextSamplerWake<=target) {
     now=nextSamplerWake;
     try { fakeTaskEntry(nullptr); assert(false); } catch(const TaskYield&) {}
   }
 }
 now=target;
}
unsigned long millis() { return now; }
void delay(unsigned long value) { advanceTime(value); }
static int physical = 0;
#define WIRE_PIN_1 0
#define WIRE_PIN_2 1
#define WIRE_PIN_3 2
#define WIRE_PIN_4 3
#define LOW 0
#define INPUT_PULLUP 2
void pinMode(int,int) {}
static void (*gpioReadHook)() = nullptr;
int digitalRead(int pin) {
 assert(criticalDepth==0);
 if(gpioReadHook) { auto hook=gpioReadHook; gpioReadHook=nullptr; hook(); }
 return pin < physical ? LOW : 1;
}
JsonDocument my;
bool batteryFinishDone=false, batteryFinishAudioPlayed=false, receiveMineOn=false;
int completions=0, refreshes=0, activations=0, renders=0, audio=0;
int WireCountPlugged();
void WirePollMain();
void WireObserveServerSnapshot();
void WireResetTracking();
void WaitFunc() {}
void BatteryFinish() { ++completions; }
void (*ptrCurrentMode)() = WirePollMain;
void BatteryPackSend() { assert(criticalDepth==0); ++renders; }
void Mp3PlayLargeFolder(int,int) { assert(criticalDepth==0); ++audio; }
#define MP3_WAIT_TIMEOUT_MS 2000
static bool dfPlayerReady=true, floodAudioEvents=false;
static const int DFPlayerPlayFinished=5;
static std::vector<int> dfEvents;
static size_t dfRead=0;
static int dfAvailableCalls=0;
struct FakePlayer {
 bool available() { assert(criticalDepth==0); ++dfAvailableCalls; return floodAudioEvents || dfRead<dfEvents.size(); }
 int readType() { assert(criticalDepth==0); return floodAudioEvents ? 1 : dfEvents.at(dfRead++); }
} myDFPlayer;
void SyncBatteryPackCur() {}
void DataChanged() { WireObserveServerSnapshot(); }
void ActivateFunc() { ++activations; ptrCurrentMode=WirePollMain; }
struct Logger {
 void println(const char*) { assert(criticalDepth==0); }
 void printf(const char*,...) { assert(criticalDepth==0); }
} Serial;
static GeneratorWireSnapshot serverState;
static const char EPOCH1[]="11111111-1111-1111-1111-111111111111";
static const char EPOCH2[]="22222222-2222-2222-2222-222222222222";
struct WifiLibrary {
 void ReceiveMine() {
   ++refreshes;
   my["device_name"]=serverState.device;
   my["game_state"]=serverState.gameState;
   my["device_state"]=serverState.deviceState;
   my["battery_pack"]=serverState.count;
   my["max_battery_pack"]=serverState.maximum;
 }
} has2wifi;
static int readRequests=0;
struct Write { int count; uint32_t revision; std::string epoch; };
static std::vector<Write> writes;
enum SendMode { Normal, FailNoApply, FailAfterApply, WrongEpoch, WrongRevision, WrongCount, Conflict };
static SendMode nextSend=Normal;
static uint32_t httpDelay=0;
static bool removeDuringHttp=false, resetDuringHttp=false;
GeneratorWireResult WireHttpRequest(bool set,const char *name,int count,const char *epoch,uint32_t revision,GeneratorWireSnapshot &reply) {
 assert(criticalDepth==0);
 advanceTime(httpDelay/2);
 if(set && removeDuringHttp) { physical=2; removeDuringHttp=false; }
 if(set && resetDuringHttp) { WireResetTracking(); resetDuringHttp=false; }
 advanceTime(httpDelay-httpDelay/2);
 assert(strcmp(name,serverState.device)==0);
 if(!set) { ++readRequests; reply=serverState; return GeneratorWireResult::Ok; }
 writes.push_back({count,revision,epoch});
 SendMode mode=nextSend; nextSend=Normal;
 if(mode==FailNoApply) return GeneratorWireResult::Failed;
 if(mode==Conflict || strcmp(epoch,serverState.epoch)!=0 || revision!=serverState.revision) {
   reply=serverState; return GeneratorWireResult::Conflict;
 }
 serverState.count=count; ++serverState.revision; reply=serverState;
 if(mode==FailAfterApply) return GeneratorWireResult::Failed;
 if(mode==WrongEpoch) strcpy(reply.epoch,EPOCH2);
 if(mode==WrongRevision) reply.revision=revision;
 if(mode==WrongCount) reply.count=count==0?1:0;
 return GeneratorWireResult::Ok;
}

'''

HTTP = r'''
struct String : std::string {
 using std::string::string;
 String(const std::string &text):std::string(text) {}
 String(int number):std::string(std::to_string(number)) {}
 String(unsigned number):std::string(std::to_string(number)) {}
};
#define WL_CONNECTED 3
struct Radio {
 int status() { return WL_CONNECTED; }
 String macAddress() { return "AA:BB:CC:DD:EE:FF"; }
} WiFi;
static const char GENERATOR_SERVER_URL[]="http://127.0.0.1";
static std::string httpBody;
static int httpStatus=200,declaredLength=-1,connectTimeout=0,readTimeout=0;
static bool streamStalled=false;
struct FakeStream {
 size_t position=0;
 int available() { return streamStalled ? 0 : static_cast<int>(httpBody.size()-position); }
 int read(uint8_t *buffer,size_t size) {
   size=std::min(size,httpBody.size()-position);
   memcpy(buffer,httpBody.data()+position,size); position+=size; return static_cast<int>(size);
 }
};
struct HTTPClient {
 FakeStream stream;
 void setConnectTimeout(int value) { connectTimeout=value; }
 void setTimeout(int value) { readTimeout=value; }
 void setReuse(bool) {}
 bool begin(const String&) { return true; }
 int GET() { return httpStatus; }
 void end() {}
 int getSize() { return declaredLength<0 ? static_cast<int>(httpBody.size()) : declaredLength; }
 bool connected() { return true; }
 FakeStream *getStreamPtr() { return &stream; }
};

'''

TESTS = r'''
static void reset(int count=0,int maximum=3) {
 now=0; physical=count; my.clear();
 fakeTaskEntry=nullptr; pauseSampler=failSamplerCreation=false; criticalDepth=0; nextSamplerWake=0;
 serverState=GeneratorWireSnapshot();
 strcpy(serverState.device,"LG"); strcpy(serverState.epoch,EPOCH1);
 strcpy(serverState.gameState,"activate"); strcpy(serverState.deviceState,"activate");
 serverState.count=count; serverState.maximum=maximum; serverState.revision=7;
 has2wifi.ReceiveMine(); refreshes=0;
 wireState=GeneratorWireState(); wireEnabled=false; wireContextValid=false;
 wireRetryPending=false; wireFenceNeeded=true; wireLastAttempt=0; wireContext=GeneratorWireSnapshot();
 memset(wireObservedGame,0,sizeof(wireObservedGame));
 memset(wireObservedDevice,0,sizeof(wireObservedDevice)); memset(wireObservedName,0,sizeof(wireObservedName));
 writes.clear(); readRequests=activations=renders=audio=completions=0;
 nextSend=Normal; httpDelay=0; removeDuringHttp=resetDuringHttp=false; batteryFinishDone=batteryFinishAudioPlayed=receiveMineOn=false;
 ptrCurrentMode=WirePollMain; wireSamplerTask=nullptr; wireSamplingEnabled=false; wireLastPresented=-1;
 gpioReadHook=nullptr; dfPlayerReady=true; floodAudioEvents=false; dfEvents.clear(); dfRead=0; dfAvailableCalls=0;
 WireInit(); WireObserveServerSnapshot();
}
static void tick(uint32_t elapsed=100) { advanceTime(elapsed); WireServiceLoop(); }
static void run(unsigned milliseconds) {
 for(unsigned i=0;i<milliseconds;i+=100) { tick(); }
}
static void settle(int count) { physical=count; run(3200); }
static std::string snapshotJson() {
 JsonDocument json; json["success"]=true; json["device_name"]="LG";
 json["battery_pack"]=3; json["max_battery_pack"]=3;
 json["game_state"]="activate"; json["device_state"]="activate";
 json["epoch"]=EPOCH1; json["revision"]=8;
 std::string result; serializeJson(json,result); return result;
}
int main() {
 reset(); run(1200); assert(wireState.synced() && writes.size()==1 && readRequests==1);
 assert(writes[0].count==0 && serverState.revision==8); // startup fences an earlier boot
 writes.clear(); readRequests=0;
 for(unsigned duration=100;duration<=900;duration+=100) {
   physical=2; run(duration); physical=0; run(1200);
 }
 assert(writes.empty() && readRequests==0);
 puts("PASS 100..900 ms round trips produce no writes; initial boot is fenced");

 for(int value=0;value<=4;++value) {
   reset(); run(1200); writes.clear(); readRequests=0;
   physical=value; run(900); assert(writes.empty()); run(2400);
   assert(wireState.stable()==value && wireState.synced());
   assert(writes.size()==(value?1u:0u));
   size_t confirmed=writes.size(); run(6000); assert(writes.size()==confirmed && readRequests==0);
 }
 puts("PASS 0..4 absolute counts, one stable update, no repeated confirmed-value requests");

 reset(4,3); run(1200); assert(WireReadyForCompletion());
 writes.clear(); settle(3); assert(WireReadyForCompletion() && serverState.count==3);
 physical=2; tick(); assert(!WireReadyForCompletion());
 run(3200); assert(serverState.count==2 && !WireReadyForCompletion());
 assert(writes.size()==2 && writes[0].count==3 && writes[1].count==2);
 puts("PASS LG physical 4->3->2 is not clamped to maximum 3 and removal blocks completion");

 reset(); run(1200); writes.clear(); physical=3; nextSend=FailAfterApply;
 run(2100); assert(writes.size()==1 && !wireState.acknowledged());
 const uint32_t failedAt=wireLastAttempt;
 while(now-failedAt<1900) { tick(); }
 assert(writes.size()==1);
 run(1500); assert(writes.size()==2 && writes.back().count==3 && wireState.synced());
 assert(wireLastAttempt-failedAt>=2000 && writes[1].revision>writes[0].revision);
 puts("PASS lost ACK retries unchanged physical count; matching GET still receives CAS fence");

 reset(); run(1200); writes.clear(); physical=3; nextSend=FailNoApply; run(2100);
 assert(writes.size()==1); physical=2; run(4000);
 assert(writes.size()==2 && writes.back().count==2 && serverState.count==2);
 const Write old=writes.front(); assert(old.revision<serverState.revision);
 puts("PASS retry uses latest stable count; previous request revision is fenced");

 for(SendMode bad : {WrongEpoch,WrongRevision,WrongCount,Conflict}) {
   reset(); run(1200); physical=3; nextSend=bad; run(2100);
   assert(!wireState.acknowledged() && !WireReadyForCompletion());
 }
 puts("PASS wrong identity context/count/revision and conflict cannot acknowledge completion");

 reset(3); run(1200); assert(WireReadyForCompletion());
 pauseSampler=true; tick(300); assert(!WireReadyForCompletion());
 const GeneratorWireState stopped=WireSnapshot();
 for(int i=0;i<5;++i) { assert(!WireReadyForCompletion()); }
 assert(stopped.generation()==WireSnapshot().generation());
 pauseSampler=false; tick(); run(800); assert(!WireCanProgress()); tick(); assert(WireReadyForCompletion());
 puts("PASS stopped sampler fails closed; main checks do not refresh heartbeat or interpolate gaps");

 reset(3); run(1200); assert(WireReadyForCompletion());
 physical=2; assert(!WireReadyForCompletion()); physical=3;
 assert(!WireReadyForCompletion()); run(900); assert(!WireCanProgress()); run(200); assert(WireReadyForCompletion());
 puts("PASS sub-sample removal observed by main requires a new real one-second window");

 reset(3); wireSamplerTask=nullptr; fakeTaskEntry=nullptr; failSamplerCreation=true;
 WireInit(); run(3000); assert(!WireCanProgress() && writes.empty());
 puts("PASS sampler creation failure cannot complete or publish raw inputs");

 reset(3); gpioReadHook=WireResetTracking;
 advanceTime(0); assert(WireSnapshot().raw()==-1 && !WireCanProgress());
 tick(); assert(WireSnapshot().raw()==3 && !WireCanProgress());
 run(900); assert(!WireCanProgress()); tick(); assert(WireReadyForCompletion());
 puts("PASS GPIO read overlapping reset discards the previous generation observation");

 reset(); run(1200); writes.clear();
 physical=2; advanceTime(1100); physical=4; advanceTime(1100);
 WireServiceLoop(); assert(writes.size()==1 && writes[0].count==4 && WireReadyForCompletion());
 puts("PASS main stall consumes only latest stable count without replaying intermediate updates");

 reset(3); run(1200); dfEvents={DFPlayerPlayFinished,1};
 Mp3BatteryStart(); const int playedBefore=audio;
 assert(!Mp3BatteryFinished() && audio==playedBefore+1 && dfRead==2);
 physical=2; advanceTime(1100); assert(WireSnapshot().stable()==2 && !WireReadyForCompletion());
 assert(!Mp3BatteryFinished()); dfEvents.push_back(1); assert(!Mp3BatteryFinished());
 dfEvents.push_back(DFPlayerPlayFinished); assert(Mp3BatteryFinished());
 assert(audio==playedBefore+1);
 puts("PASS actual audio phase drains queued finish and sampler keeps observing while audio is pending");

 Mp3BatteryStart(); assert(!Mp3BatteryFinished());
 advanceTime(1999); assert(!Mp3BatteryFinished()); advanceTime(1); assert(Mp3BatteryFinished());
 Mp3BatteryStart(); assert(!Mp3BatteryFinished()); WireResetTracking();
 assert(batteryAudioPhase==BatteryAudioPhase::Idle && Mp3BatteryFinished());
 dfPlayerReady=false; Mp3BatteryStart(); assert(Mp3BatteryFinished());
 dfPlayerReady=true; floodAudioEvents=true; Mp3BatteryStart(); dfAvailableCalls=0;
 assert(!Mp3BatteryFinished() && dfAvailableCalls==8);
 advanceTime(2000); assert(Mp3BatteryFinished()); floodAudioEvents=false;
 puts("PASS actual audio timeout, round cancellation, unavailable player and bounded event flood");

 reset(3); httpDelay=500;
 while(!WireCanProgress() && now<4000) { tick(); }
 assert(now==2000 && WireReadyForCompletion());
 const uint32_t acknowledgedAt=wireLastAttempt;
 physical=4; while(serverState.count!=4 && now<5000) { tick(); }
 assert(wireLastAttempt-acknowledgedAt==1600 && WireReadyForCompletion());
 puts("PASS latency: stable at 1000 ms, GET/SET 500 ms each -> ready at 2000 ms; next stable send has no 2-second gate");
 physical=3; removeDuringHttp=true;
 while(writes.back().count!=3 && now<8000) { tick(); }
 assert(!WireReadyForCompletion()); run(2500); assert(serverState.count==2 && !WireReadyForCompletion());
 puts("PASS sampler observes removal during synchronous HTTP and latest count replaces old ACK");

 reset(3); httpDelay=500; resetDuringHttp=true;
 while(writes.empty() && now<4000) { tick(); }
 assert(!wireState.acknowledged() && wireRetryPending);
 puts("PASS reset during HTTP rejects an old generation ACK");

 reset(3); run(1200); assert(WireReadyForCompletion());
 strcpy(serverState.gameState,"setting"); has2wifi.ReceiveMine(); DataChanged();
 assert(!wireEnabled && !WireCanProgress() && !wireState.acknowledged());
 strcpy(serverState.epoch,EPOCH2); strcpy(serverState.gameState,"activate"); serverState.count=0;
 has2wifi.ReceiveMine(); DataChanged(); run(900); assert(!WireCanProgress()); run(4500);
 assert(wireState.synced() && serverState.count==3);
 puts("PASS setting/new round cancels old qualification and acknowledgement");

 reset(3); run(1200); batteryFinishDone=true; batteryFinishAudioPlayed=true;
 strcpy(serverState.deviceState,"battery_max"); ++serverState.revision;
 has2wifi.ReceiveMine(); DataChanged(); assert(!wireContextValid && batteryFinishDone && batteryFinishAudioPlayed);
 size_t before=writes.size(); run(2500);
 assert(wireContextValid && writes.size()==before && batteryFinishDone && batteryFinishAudioPlayed);
 puts("PASS normal battery_max echo refreshes revision without redoing writes or audio");

 for(const char *completed : {"repaired","repaired_all"}) {
   reset(3); strcpy(serverState.deviceState,completed); has2wifi.ReceiveMine(); ptrCurrentMode=WaitFunc;
   run(1200); strcpy(serverState.epoch,EPOCH2); serverState.revision=1;
   physical=2; run(4500);
   assert(activations==0 && ptrCurrentMode==WaitFunc && std::string(my["device_state"].as<const char*>())==completed);
 }
 puts("PASS new epoch never reactivates repaired/repaired_all generators");

 reset(3); serverState.count=9; has2wifi.ReceiveMine(); run(1200);
 assert(serverState.count==3 && wireState.synced());
 puts("PASS legacy out-of-range accumulated count is corrected to physical absolute count");
 reset(3); run(1200); serverState.revision=0; strcpy(serverState.device,"BG");
 has2wifi.ReceiveMine(); DataChanged(); run(3500);
 assert(wireState.synced() && strcmp(wireContext.device,"BG")==0 && serverState.count==3);
 puts("PASS new device identity does not inherit the previous device revision");


 // Actual JSON parser and bounded HTTP body reader, against a fake TCP stream.
 reset(); GeneratorWireSnapshot parsed; JsonDocument json;
 std::string valid=snapshotJson(); deserializeJson(json,valid);
 assert(WireParseSnapshot(json,"LG",parsed));
 for(const char *field : {"battery_pack","max_battery_pack","revision"}) {
   deserializeJson(json,valid); json[field]="3"; assert(!WireParseSnapshot(json,"LG",parsed));
   deserializeJson(json,valid); json[field]=true; assert(!WireParseSnapshot(json,"LG",parsed));
   deserializeJson(json,valid); json[field]=3.5; assert(!WireParseSnapshot(json,"LG",parsed));
 }
 deserializeJson(json,valid); json["battery_pack"]=-9; assert(WireParseSnapshot(json,"LG",parsed));
 json["battery_pack"]=int64_t(2147483648LL); assert(!WireParseSnapshot(json,"LG",parsed));
 deserializeJson(json,valid); json["revision"]=uint64_t(4294967296ULL); assert(!WireParseSnapshot(json,"LG",parsed));
 deserializeJson(json,valid); json["epoch"]="old"; assert(!WireParseSnapshot(json,"LG",parsed));
 deserializeJson(json,valid); json["device_name"]="BG"; assert(!WireParseSnapshot(json,"LG",parsed));
 httpBody=valid; httpStatus=200; declaredLength=-1; streamStalled=false;
 assert(ActualWireHttpRequest(true,"LG",3,EPOCH1,7,parsed)==GeneratorWireResult::Ok);
 assert(connectTimeout==700 && readTimeout==700);
 assert(ActualWireHttpRequest(true,"LG",2,EPOCH1,7,parsed)==GeneratorWireResult::Failed);
 assert(ActualWireHttpRequest(true,"LG",3,EPOCH1,8,parsed)==GeneratorWireResult::Failed);
 deserializeJson(json,valid); json["success"]=false; json["error"]="CONFLICT";
 httpBody.clear(); serializeJson(json,httpBody); httpStatus=409;
 assert(ActualWireHttpRequest(true,"LG",3,EPOCH1,7,parsed)==GeneratorWireResult::Conflict);
 httpStatus=200; assert(ActualWireHttpRequest(false,"LG",0,"",0,parsed)==GeneratorWireResult::Failed);
 httpBody=valid; declaredLength=1025;
 assert(ActualWireHttpRequest(false,"LG",0,"",0,parsed)==GeneratorWireResult::Failed);
 declaredLength=-1; streamStalled=true; uint32_t start=now;
 assert(ActualWireHttpRequest(false,"LG",0,"",0,parsed)==GeneratorWireResult::Failed);
 assert(now-start==1000);
 puts("PASS actual strict JSON/ACK parser, conflicts, body cap and body-read deadline");
 return 0;
}

'''
with tempfile.TemporaryDirectory(prefix="generator-wire-test-") as directory:
    temp = Path(directory)
    (temp/"HTTPClient.h").write_text("// Fake HTTPClient is defined by the harness.\n")
    audio_source = (ROOT/"dfplayer.ino").read_text().split('enum class BatteryAudioPhase',1)[1]
    source = PREFIX + '\nenum class BatteryAudioPhase' + audio_source + '\n' + (ROOT/"wire.ino").read_text() + '\n' + HTTP
    source += '\n#define WireHttpRequest ActualWireHttpRequest\n' + (ROOT/"wire_http.ino").read_text()
    source += '\n#undef WireHttpRequest\n' + TESTS
    (temp/"test.cpp").write_text(source)
    subprocess.run([os.environ.get("CXX","c++"),"-std=c++11","-Wall","-Wextra","-Werror",
                    "-fsanitize=address,undefined","-fno-omit-frame-pointer",
                    "-I",str(ROOT),"-I",include,"-I",str(temp),str(temp/"test.cpp"),"-o",str(temp/"test")],check=True)
    subprocess.run([str(temp/"test")],check=True)

COMPLETION_PREFIX = r'''
#include <cassert>
#include <string>
#include <map>
#include <iostream>
#define BREADCRUMB(x) ((void)0)
struct String:std::string { using std::string::string; };
struct Value { std::string text; Value& operator=(const char* value) {text=value;return *this;} operator const char*() const {return text.c_str();} };
struct Document { std::map<std::string,Value> values; Value& operator[](const char* key){return values[key];} } my;
struct Logger { void println(const char*) {} } Serial;
static bool ready=true, removeDuringAudio=false, removeDuringSend=false, audioFinished=true;
bool WireReadyForCompletion() { return ready; }
bool batteryFinishDone=false,batteryFinishAudioPlayed=false,receiveMineOn=false,starterRfidNeedsValidation=false;
static int played=0,sent=0,received=0,detached=0,started=0;
long encoderValue=1000;
int displayedGaugeNeoCnt=-1,gameTimerCnt=0,gameTimerId=0,blinkTimerId=0,gameTime=400;
void GameTimerFunc() {}
struct Timer {
 void deleteTimer(int) {}
 int setInterval(int,void(*)()) {++started;return 1;}
 bool isEnabled(int) {return true;}
} GameTimer,BlinkTimer;
void WaitFunc() {}
void StarterActivate() {}
void BatteryFinish();
void (*ptrCurrentMode)()=BatteryFinish;
void (*ptrRfidMode)()=WaitFunc;
void EncoderDetach() {++detached;}
int StarterGaugeCnt() {return 1;}
void EncoderNeopixelOn(int) {}
enum {GAUGE,STARTER,DEVICESTATE,CIRCUIT,BLUE};
int color[5][3];
void NeoLightColor(int,int*) {}
void AllNeoOn(int) {}
void LeftGenerator() {}
void Mp3PlayLargeFolder(int,int) {}
void Mp3PlayLargeFolderAndWait(int,int) {++played;if(removeDuringAudio)ready=false;}
void Mp3BatteryStart() {++played;if(removeDuringAudio)ready=false;}
bool Mp3BatteryFinished() {return audioFinished;}
struct Wifi {
 void Send(String,const char*,const char*) {++sent;if(removeDuringSend)ready=false;}
 void ReceiveMine() {++received;}
} has2wifi;
'''
COMPLETION_TESTS = r'''
int main() {
 my["device_name"]="LG";my["device_state"]="activate";
 ready=false;BatteryFinish();assert(sent==0&&played==0&&!batteryFinishDone);
 ready=true;audioFinished=false;BatteryFinish();
 assert(played==1&&sent==0&&!batteryFinishDone);
 BatteryFinish();assert(played==1&&sent==0);
 audioFinished=true;ready=false;BatteryFinish();
 assert(played==1&&sent==0&&batteryFinishAudioPlayed&&!batteryFinishDone);
 BatteryFinish();assert(played==1&&sent==0);
 ready=true;removeDuringAudio=false;removeDuringSend=true;BatteryFinish();
 assert(played==1&&sent==1&&batteryFinishDone&&ptrCurrentMode==BatteryFinish);
 ready=true;removeDuringSend=false;BatteryFinish();
 assert(played==1&&sent==1&&ptrCurrentMode==StarterActivate);
 std::cout<<"PASS actual BatteryFinish checks again after audio and HTTP; announcement/send are not repeated\n";
 sent=0;ready=false;StartFinish();assert(sent==0&&ptrCurrentMode==StarterActivate);
 ready=true;removeDuringSend=true;StartFinish();
 assert(sent==1&&received==1&&ptrCurrentMode==WaitFunc);
 std::cout<<"PASS actual StartFinish blocks unconfirmed inputs and does not undo an already-sent repair\n";
}
'''

def production_function(file, name):
    source = (ROOT/file).read_text()
    start = source.index('void '+name+'(')
    opening = source.index('{',start)
    depth, end = 1, opening+1
    while depth:
        depth += (source[end]=='{') - (source[end]=='}')
        end += 1
    return source[start:end]

with tempfile.TemporaryDirectory(prefix="generator-wire-completion-") as directory:
    temp=Path(directory)
    source=COMPLETION_PREFIX+'\n'+production_function('rfid.ino','BatteryFinish')+'\n'+production_function('rfid.ino','StartFinish')+'\n'+COMPLETION_TESTS
    (temp/'test.cpp').write_text(source)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer',str(temp/'test.cpp'),'-o',str(temp/'test')],check=True)
    subprocess.run([str(temp/'test')],check=True)
