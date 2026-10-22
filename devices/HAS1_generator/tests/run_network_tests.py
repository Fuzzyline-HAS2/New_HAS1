#!/usr/bin/env python3
"""Compile actual worker/facade and wire adapter with controlled scheduling.
No actual network, FreeRTOS scheduling or GPIO timing is claimed by this harness.
"""
from pathlib import Path
import os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
LIB=ROOT.parents[1]/'libraries/HAS2_Wifi'
JSON=Path(os.environ['ARDUINOJSON_INCLUDE'])
network=(ROOT/'network.ino').read_text().replace('#include <esp_task_wdt.h>','')
contrib=(ROOT/'contrib.ino').read_text()
wifi=(ROOT/'wifi.ino').read_text()
actual_datachanged=wifi[wifi.index('JsonDocument cur;'):wifi.index('// 아무 동작도 하지 않는 대기 콜백.')].replace('void DataChanged()', 'void ProductionDataChanged()')
wire=(ROOT/'wire_http.ino').read_text().replace('#include <HTTPClient.h>','').replace('#include <has2_http_body.h>','')
prefix=r'''
#define HAS2_BT_HOST
#include <ArduinoJson.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <cstdio>
#include <utility>
struct String:std::string {
 using std::string::string;
 String()=default;String(const std::string &s):std::string(s){};
 String(int n):std::string(std::to_string(n)){};
 String(unsigned n):std::string(std::to_string(n)){};
};
#include "generator_network.h"
#include "generator_wire_state.h"
#include "generator_wire_protocol.h"
static uint32_t now=100;
static bool worker=false,online=true,failWatchdog=false;
static int depth=0,restarts=0,dataChanges=0,otaChecks=0,requestCalls=0;
uint32_t millis(){return now;}
using TaskHandle_t=void*;using BaseType_t=int;using esp_err_t=int;using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) do{(void)(x);assert(++depth==1);}while(0)
#define portEXIT_CRITICAL(x) do{(void)(x);assert(--depth==0);}while(0)
#define pdMS_TO_TICKS(x) (x)
#define pdPASS 1
#define ESP_OK 0
#define WL_CONNECTED 3
#define FIRMWARE_VER 27
struct Yield{};
void vTaskDelay(uint32_t ms){assert(worker&&depth==0);now+=ms;throw Yield();}
void vTaskDelete(void*){throw Yield();}
int esp_task_wdt_add(void*){return failWatchdog?-1:0;}
int esp_task_wdt_reset(){assert(worker||!worker);return 0;}
int esp_task_wdt_delete(void*){return 0;}
int xTaskCreatePinnedToCore(void(*)(void*),const char*,unsigned,void*,int,TaskHandle_t *task,int){*task=(void*)1;return pdPASS;}
struct Wifi{int status(){return online?3:0;}}WiFi;
struct Esp{void restart(){assert(!worker);++restarts;}}ESP;
struct Ota{void check(){assert(!worker);++otaChecks;}}ota;
JsonDocument my,tag,shift_machine;
bool receiveMineOn=false;
void ContribEnd(int);
static bool exerciseActualDataChanged=false;
void ProductionDataChanged();
void DataChanged(){assert(!worker);++dataChanges;if(exerciseActualDataChanged)ProductionDataChanged();}
void WaitFunc(){}
static int activations=0;
void ActivateFunc(){++activations;}
void SettingFunc(){}
void ReadyFunc(){}
void EncoderDetach(){}
void (*ptrCurrentMode)()=WaitFunc;
struct HAS2_WifiDisconnectDiagnostic {uint32_t sequence=0,atMs=0;uint8_t reason=0;};
uint32_t uxTaskGetStackHighWaterMark(void*){return 4096;}
struct Has {
 HAS2_WifiDisconnectDiagnostic GetDisconnectDiagnostic(){assert(!worker);return {};}
 String GetMacAddress(){return "00:11:22:33:44:55";}
 bool ServiceConnection(){assert(worker);return online;}
 bool ConnectionScanPending(){return false;}
 bool ConnectionDriverFault(){return false;}
 bool ApplyNetworkSnapshot(const char *kind,const String &body){
   assert(!worker);JsonDocument doc;if(deserializeJson(doc,body.c_str()))return false;
   if(!strcmp(kind,"Loop"))return shift_machine.set(doc);
   if(!strcmp(kind,"ReceiveMine"))return my.set(doc);
   return tag.set(doc);
 }
 void FirmwareUpdate(const char*,const char*){++otaChecks;}
}has2wifi;
namespace GeneratorDiagnostic{enum Kind{ServerReset,Has2Http};}
void DiagnosticPoint(GeneratorDiagnostic::Kind,int,uint32_t){}
#define BREADCRUMB(x) do{(void)(x);}while(0)
struct Logger { template<class T>void println(const T&){} } Serial;
int encoderValue=0,starterEncoderUnit=1,NumPixels[1]={28};
const int GAUGE=0;
String starterContribUser;int starterContribStartCnt=0,starterContribLastCnt=0;
bool starterLastTagState=false;
void StarterActivate(){}
static std::vector<std::string> diagnostics;
void DiagnosticPrintf(const char *format,...){diagnostics.emplace_back(format);}
static GeneratorWireState wireState;
GeneratorWireState WireSnapshot(){return wireState;}
static Has2BoundedTransport::Result nextReply;
static void (*duringRequest)()=nullptr;
static std::vector<std::string> paths;
namespace Has2BoundedTransport {
Result get(const char*,uint16_t,const char *path,const Options &options){
 assert(worker&&depth==0);assert(options.deadlineMs==2000);++requestCalls;paths.emplace_back(path);
 if(strstr(path,"GeneratorWires")){assert(options.bodyDeadlineMs==1000&&options.maxBodyBytes==1024&&options.requireContentLength);}
 if(options.progressHook)options.progressHook();
 if(duringRequest){auto hook=duringRequest;duringRequest=nullptr;hook();}
 now+=nextReply.timings.totalMs;
 return nextReply;
}
}
'''
application_stubs=r'''
namespace Has1BleBeacon {void setDeviceName(const char*){}}
static int gameTimerDeletes=0;
struct Timer {void deleteTimer(int){++gameTimerDeletes;}} GameTimer,BlinkTimer;
int gameTimerId=1,blinkTimerId=2,starterDecreaseAmount=1;
bool starterTaggerActive=false;
void (*ptrRfidMode)()=WaitFunc;
const int BLUE=0,RED=1,YELLOW=2,STARTER=0,DEVICESTATE=1,CIRCUIT=2;
int color[3]={0,1,2};
void WireObserveServerSnapshot(){}
void UpdateBrightness(){}
void LeftGenerator(){}
void TaggerReset(){starterTaggerActive=false;}
void StarterGaugeUpdate(bool){}
void Mp3PlayLargeFolderAndWait(int,int){}
void Mp3PlayLargeFolder(int,int){}
void AllNeoOn(int){}
void WirePollMain(){}
void NeoLightColor(int,int){}
void WireResetTracking(){}
void TaggerRfidLoop(){}
void TaggerEnter(){}
'''
tests=r'''
static void reply(const char *json,int status=200){nextReply=Has2BoundedTransport::Result{};nextReply.status=status;nextReply.payload=json;nextReply.timings.totalMs=100;}
static void workerTurn(){worker=true;try{NetworkWorker(nullptr);}catch(const Yield&){}worker=false;}
static void mainTurn(){GeneratorNetworkLoop();}
static int used(){int count=0;for(const auto&s:networkSlots)if(s.occupied)++count;return count;}
static void reset(){
 now=100;online=true;worker=false;failWatchdog=false;requestCalls=restarts=dataChanges=otaChecks=0;depth=0;
 exerciseActualDataChanged=false;receiveMineOn=false;ptrCurrentMode=WaitFunc;cur.clear();gameTimerDeletes=0;
 paths.clear();diagnostics.clear();my.clear();tag.clear();shift_machine.clear();
 for(auto &s:networkSlots)s=NetworkSlot{};
 networkTask=(void*)1;networkGeneration=1;networkNextId=1;networkMailboxReady=false;networkMailboxSlot=-1;networkMailboxCancelled=false;
 networkMailbox=Has2BoundedTransport::Result{};
 networkPollPending=networkPollEvent=networkPollSuccess=networkPollAwaitingRefresh=false;networkUrgentRun=0;networkWorkerActiveId=0;networkScanPending=networkDriverFault=false;networkRefreshPending=networkRefreshInFlight=networkResetCyclePending=false;activations=0;networkBootReady=true;
 networkVersionSent=true;networkWatchdogPending=networkWatchdogRequested=false;networkWasConnected=true;
 networkMaintenance=networkMaintenanceLegacy=false;networkRefreshAt=0;networkIdentity[0]=networkGame[0]=0;
 pendingContributionCount=0;starterContribUser="";
 networkCompletionGeneration=0;networkCompletion[0]=0;networkCompletionSent=networkCompletionVerify=networkCompletionConfirmed=false;
 GeneratorNetworkForgetTag();wireHttpPending=wireHttpDone=false;wireHttpRequestId=0;
 wireState.reset();for(int i=0;i<=10;++i)wireState.sampleMask(i*100,0);
 my["device_name"]="TG";my["game_state"]="activate";my["device_state"]="activate";
 strcpy(networkIdentity,"TG");strcpy(networkGame,"activate");
 reply("{}");
}
static void pass(const char *text){printf("PASS %s\n",text);}
int main(){
 reset();assert(GeneratorNetworkRequestPoll());for(int i=0;i<1000;++i)assert(GeneratorNetworkRequestPoll());
 assert(used()==1&&requestCalls==0);reply("{\"shift_machine\":0,\"watchdog\":0}");workerTurn();assert(requestCalls==1&&dataChanges==0);
 mainTurn();bool ok=false;assert(GeneratorNetworkTakePollResult(ok)&&ok&&!GeneratorNetworkPollBusy());
 pass("poll coalescing and worker/main ownership; no transport on enqueue");

 reset();networkRefreshPending=true;assert(NetworkQueueRefresh());assert(GeneratorNetworkRequestPoll());
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"activate\"}");workerTurn();mainTurn();
 assert(networkPollPending&&!networkPollEvent);reply("{\"shift_machine\":0,\"watchdog\":0}");workerTurn();mainTurn();
 assert(!networkPollPending&&GeneratorNetworkTakePollResult(ok)&&ok);
 pass("independent refresh result cannot complete a still-queued poll");

 reset();for(int i=0;i<6;++i)assert(GeneratorNetworkSend("TG","crash_log","x"));assert(GeneratorNetworkRequestPoll());
 for(int i=0;i<4;++i){reply("{\"shift_machine\":0,\"watchdog\":0}");workerTurn();mainTurn();}
 assert(paths.size()==4&&paths[3].find("request=Loop")!=std::string::npos);
 pass("heartbeat is served after at most three other completed operations");

 reset();online=false;for(int i=0;i<6;++i)assert(GeneratorNetworkSend("TG","crash_log","x"));
 assert(!GeneratorNetworkSend("TG","crash_log","overflow"));assert(GeneratorNetworkRequestPoll());assert(used()==7);
 for(int i=0;i<500;++i)workerTurn();assert(used()==7&&requestCalls==0);
 online=true;reply("{}");workerTurn();assert(networkMailboxReady&&requestCalls==0);mainTurn();workerTurn();assert(networkMailboxReady&&requestCalls==1);for(int i=0;i<20;++i)workerTurn();assert(requestCalls==1);
 pass("bounded optional queue reserves heartbeat capacity; offline and unread result cannot spawn work");

 reset();reply("{\"role\":\"player\"}");assert(!GeneratorNetworkTag("G1P1"));workerTurn();assert(tag.isNull());
 GeneratorNetworkForgetTag();mainTurn();assert(tag.isNull());
 assert(!GeneratorNetworkTag("G1P2"));nextReply.error=Has2BoundedTransport::Error::Deadline;workerTurn();mainTurn();
 int calls=requestCalls;for(int i=0;i<100;++i)assert(!GeneratorNetworkTag("G1P2"));assert(requestCalls==calls&&used()==0);
 now+=8001;reply("{\"role\":\"player\"}");assert(!GeneratorNetworkTag("G1P2"));workerTurn();mainTurn();assert(GeneratorNetworkTag("G1P2"));
 assert(!strcmp(tag["role"].as<const char*>(),"player"));
 pass("old card result discarded; same held card retries after bounded backoff");

 reset();for(int i=0;i<100;++i){GeneratorNetworkForgetTag();assert(!GeneratorNetworkTag("G1P1"));}
 assert(used()==1);assert(GeneratorNetworkRequestPoll());assert(used()==2);
 pass("card replacement cancels queued old lookups without starving heartbeat capacity");

 reset();assert(GeneratorNetworkRequestPoll());reply("{\"shift_machine\":1,\"watchdog\":0}");workerTurn();mainTurn();assert(networkRefreshInFlight&&networkPollPending);
 nextReply.error=Has2BoundedTransport::Error::Truncated;workerTurn();mainTurn();assert(networkRefreshPending&&!networkPollPending);
 assert(GeneratorNetworkRequestPoll());reply("{\"shift_machine\":0,\"watchdog\":0}");workerTurn();mainTurn();assert(networkRefreshInFlight);
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"activate\"}");workerTurn();mainTurn();
 assert(!networkRefreshPending&&dataChanges==1);
 pass("lost ReceiveMine response remains pending after server notification is consumed");
 reset();GeneratorNetworkRefreshCycle();mainTurn();reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"activate\"}");
 workerTurn();mainTurn();assert(activations==1&&!networkResetCyclePending);
 pass("a new CAS epoch forces fresh cycle initialization even when state text is unchanged");
 for(const char *state:{"repaired","repaired_all","player_win","player_lose"}) {
  reset();my["device_state"]=state;GeneratorNetworkRefreshCycle();mainTurn();
  reply((std::string("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"")+state+"\"}").c_str());
  workerTurn();mainTurn();assert(activations==0&&!networkResetCyclePending&&ptrCurrentMode==WaitFunc);
 }
 pass("new epoch does not reactivate completed or terminal states");


 reset();GeneratorWireSnapshot snapshot;
 assert(WireHttpRequest(false,"TG",0,"",0,snapshot)==GeneratorWireResult::Pending);
 reply("{\"device_name\":\"WRONG\",\"epoch\":\"11111111-1111-1111-1111-111111111111\",\"game_state\":\"activate\",\"device_state\":\"activate\",\"revision\":1,\"battery_pack\":0,\"max_battery_pack\":4,\"success\":true}");
 workerTurn();mainTurn();assert(WireHttpRequest(false,"TG",0,"",0,snapshot)==GeneratorWireResult::Failed);
 pass("wire response cannot overwrite expected identity before validation");

 reset();assert(WireHttpRequest(true,"TG",0,"11111111-1111-1111-1111-111111111111",1,snapshot)==GeneratorWireResult::Pending);
 wireState.reset();workerTurn();assert(requestCalls==0);mainTurn();
 assert(WireHttpRequest(true,"TG",0,"11111111-1111-1111-1111-111111111111",1,snapshot)==GeneratorWireResult::Failed);
 pass("queued stale physical-generation SET is rejected before transport");

 reset();assert(WireHttpRequest(true,"TG",0,"11111111-1111-1111-1111-111111111111",1,snapshot)==GeneratorWireResult::Pending);
 for(int i=11;i<22;++i)wireState.sampleMask(i*100,1);
 assert(WireHttpRequest(true,"TG",1,"11111111-1111-1111-1111-111111111111",1,snapshot)==GeneratorWireResult::Pending);
 workerTurn();assert(requestCalls==1&&paths[0].find("value=1")!=std::string::npos);
 pass("queued wire count coalesces to the latest qualified count");

 reset();
#if GENERATOR_ALLOW_UNFENCED_COMPLETION
 assert(GeneratorNetworkContribution("G1P1",2));
#else
 assert(!GeneratorNetworkContribution("G1P1",2));
 NetworkSubmit(NetworkKind::Contribution,"/test",true,GeneratorNetworkGeneration(),0,NetworkReportResult);
#endif
 GeneratorNetworkInvalidateGame();workerTurn();assert(requestCalls==0);mainTurn();
 pass("game/epoch invalidation rejects queued mutation before send");

 reset();
 for(int i=0;i<6;++i)assert(GeneratorNetworkSend("TG","crash_log","x"));
 for(int i=0;i<6;++i){ContribBegin("G1P1",0);ContribEnd(2);}
 assert(pendingContributionCount==4&&used()==6);
 GeneratorNetworkInvalidateGame();ContribLoop();assert(pendingContributionCount==3);
 pass("four unsent contribution events are retained on queue rejection; old game pending is dropped explicitly");
 reset();ContribBegin("G1P1",0);GeneratorNetworkInvalidateGame();ContribEnd(5);assert(used()==0&&pendingContributionCount==0);
 pass("an active contribution session retains its original game generation");

 reset();assert(GeneratorNetworkRequestPoll());assert(GeneratorNetworkSend("TG","crash_log","x"));
 online=false;GeneratorNetworkRequestMaintenance();workerTurn();mainTurn();workerTurn();mainTurn();assert(used()==0);
 now+=10001;mainTurn();assert(!networkMaintenance&&otaChecks==0);
 pass("maintenance cancels queued requests while offline and aborts without a blocking OTA");

 reset();assert(!GeneratorNetworkTag("G1P1"));reply("{\"role\":\"player\"}");
 duringRequest=[](){++networkGeneration;};workerTurn();mainTurn();assert(tag.isNull());
 pass("in-flight response from the previous game cannot update current role");

 reset();assert(GeneratorNetworkRequestPoll());reply("{\"shift_machine\":0,\"watchdog\":1}");workerTurn();mainTurn();assert(networkWatchdogPending);
 nextReply.error=Has2BoundedTransport::Error::Deadline;workerTurn();mainTurn();assert(restarts==0);
 assert(GeneratorNetworkRequestPoll());reply("{\"shift_machine\":0,\"watchdog\":0}");workerTurn();mainTurn();assert(restarts==1);
 pass("staff reboot survives lost clear ACK and requires confirmation");

 reset();failWatchdog=true;workerTurn();assert(!networkTask);assert(!GeneratorNetworkRequestPoll());
 pass("worker watchdog registration failure cannot leave an accepting dead queue");

 reset();
#if GENERATOR_ALLOW_UNFENCED_COMPLETION
 assert(!GeneratorNetworkComplete("battery_max"));assert(used()==1);
 nextReply.error=Has2BoundedTransport::Error::Deadline;workerTurn();mainTurn();
 assert(used()==1); // Main loop schedules verification even without another game callback.
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"battery_max\"}");workerTurn();mainTurn();
 assert(GeneratorNetworkComplete("battery_max"));assert(requestCalls==2);
 pass("bench-only completion verifies uncertain mutation and never blindly repeats Send");
 reset();my["device_state"]="battery_max";my["left_generator"]="3";cur.set(my);
 ptrCurrentMode=StarterActivate;exerciseActualDataChanged=true;encoderValue=28;tag["role"]="player";
 assert(!GeneratorNetworkComplete("repaired"));
 // Remove the card and decay the gauge before Send returns. No StartFinish reentry.
 tag.clear();encoderValue=0;nextReply.error=Has2BoundedTransport::Error::Deadline;workerTurn();mainTurn();assert(used()==1);
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"repaired\",\"left_generator\":\"3\"}");
 workerTurn();mainTurn();assert(ptrCurrentMode==WaitFunc&&gameTimerDeletes>=2&&!receiveMineOn);
 assert(!strcmp(cur["device_state"].as<const char*>(),"repaired"));assert(requestCalls==2);
 pass("actual DataChanged completes repaired after card removal and gauge decay without StartFinish reentry");
#else
 for(int i=0;i<1000;++i)assert(!GeneratorNetworkComplete("battery_max"));
 assert(requestCalls==0&&used()==0&&!diagnostics.empty());
 pass("default deployment gate rejects unfenced completion without silently sending");
#endif
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='generator-network-test-') as tmp:
 p=Path(tmp);src=p/'test.cpp';src.write_text(prefix+'\n'+application_stubs+'\n'+actual_datachanged+'\n'+network+'\n'+wire+'\n'+contrib+'\n'+tests)
 for mode in (0,1):
  exe=p/f'test-{mode}'
  subprocess.run([os.environ.get('CXX','c++'),'-std=c++11','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-fno-omit-frame-pointer',f'-DGENERATOR_ALLOW_UNFENCED_COMPLETION={mode}','-I',str(ROOT),'-I',str(LIB),'-I',str(JSON),str(src),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
