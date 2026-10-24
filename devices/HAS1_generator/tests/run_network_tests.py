#!/usr/bin/env python3
"""Compile actual worker/facade and wire adapter with controlled scheduling.
No actual network, FreeRTOS scheduling or GPIO timing is claimed by this harness.
"""
from pathlib import Path
import os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
LIB=ROOT.parents[1]/'libraries/HAS2_Wifi'
JSON=Path(os.environ['ARDUINOJSON_INCLUDE'])
network=(ROOT/'network.ino').read_text().replace('#include <esp_task_wdt.h>','').replace('#include <esp_attr.h>','').replace('#include <esp_system.h>','').replace('#include <esp_timer.h>','')
contrib=(ROOT/'contrib.ino').read_text()
wifi=(ROOT/'wifi.ino').read_text()
actual_activate=wifi[wifi.index('void ActivateFunc(void)'):wifi.index('// game_state == \"ready\"')].replace('void ActivateFunc(void)', 'void ProductionActivateFunc(void)')
rfid=(ROOT/'rfid.ino').read_text()
actual_finish=rfid[rfid.index('void BatteryFinish()'): ]
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
static std::vector<std::string> otaTrace;
static bool blackboxUploadIdle=true;
namespace has1blackbox {
 struct Logger {
  void setUploadAllowed(bool) { assert(depth==0); }
  void setPaused(bool paused) { assert(depth==0);otaTrace.emplace_back(paused?"pause":"unpause"); }
  bool uploadIdle() const { return blackboxUploadIdle; }
 } blackbox;
}
void BlackBoxNetwork(bool,bool,uint32_t,int,uint32_t,uint32_t,uint32_t) {assert(depth==0);}
void BlackBoxHttp(int,int,int,uint32_t,bool) {assert(depth==0);}
void BlackBoxHttpDetail(const NetworkRequest &,const Has2BoundedTransport::Result &,bool,int) {assert(depth==0);}
void BlackBoxRecoveryState(unsigned,bool,bool,bool,bool) {assert(depth==0);}
void BlackBoxValidSnapshot(bool) {assert(depth==0);}
void BlackBoxPollResult(bool) {assert(depth==0);}
void BlackBoxBeforeRestart() {assert(depth==0);}
static std::vector<uint32_t> otaRestartLogs;
void BlackBoxOtaRestart(uint32_t attempts) {assert(depth==0);otaRestartLogs.push_back(attempts);}
void BlackBoxOfflineRecovery(int,uint32_t,uint32_t,uint32_t) {assert(depth==0);}
uint32_t millis(){return now;}
using TaskHandle_t=void*;using BaseType_t=int;using esp_err_t=int;using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) do{(void)(x);assert(++depth==1);}while(0)
#define portEXIT_CRITICAL(x) do{(void)(x);assert(--depth==0);}while(0)
#define pdMS_TO_TICKS(x) (x)
#define pdPASS 1
#define ESP_OK 0
#define WL_CONNECTED 3
#define RTC_NOINIT_ATTR
#define ESP_RST_POWERON 1
#define ESP_RST_SW 3
#define ESP_RST_PANIC 4
#define ESP_RST_INT_WDT 5
#define ESP_RST_TASK_WDT 6
#define ESP_RST_WDT 7
static int resetReason=ESP_RST_POWERON;
int esp_reset_reason(){return resetReason;}
using esp_timer_handle_t=void*;
struct esp_timer_create_args_t {void (*callback)(void*);void *arg;int dispatch_method;const char *name;bool skip_unhandled_events;};
#define ESP_TIMER_TASK 0
static int guardCreates=0,guardArms=0,guardStops=0;
static bool guardArmed=false;
int esp_timer_create(const esp_timer_create_args_t *args,esp_timer_handle_t *handle){assert(args->callback&&args->dispatch_method==ESP_TIMER_TASK);++guardCreates;*handle=(void*)1;return 0;}
int esp_timer_start_once(esp_timer_handle_t,uint64_t us){assert(us==300000000ull&&!guardArmed);guardArmed=true;++guardArms;return 0;}
int esp_timer_stop(esp_timer_handle_t){guardArmed=false;++guardStops;return 0;}
void esp_restart(){}
#define FIRMWARE_VER 27
struct Yield{};
void vTaskDelay(uint32_t ms){assert(worker&&depth==0);now+=ms;throw Yield();}
void vTaskDelete(void*){throw Yield();}
int esp_task_wdt_add(void*){return failWatchdog?-1:0;}
int esp_task_wdt_reset(){assert(worker||!worker);return 0;}
int esp_task_wdt_delete(void*){return 0;}
int xTaskCreatePinnedToCore(void(*)(void*),const char*,unsigned,void*,int,TaskHandle_t *task,int){*task=(void*)1;return pdPASS;}
static int wifiStatusCalls=0;static bool reconnectOnRecheck=false;
struct Wifi{int status(){if(reconnectOnRecheck&&++wifiStatusCalls==2)online=true;return online?3:0;}}WiFi;
struct Esp{void restart(){assert(!worker);++restarts;} uint32_t getFreeHeap(){return 48000;} uint32_t getMaxAllocHeap(){return 38900;}}ESP;
static void (*duringOta)()=nullptr;
struct Ota{void check(){assert(!worker&&guardArmed);++otaChecks;otaTrace.emplace_back("ota");if(duringOta){duringOta();}}}ota;
JsonDocument my,tag,shift_machine;
bool receiveMineOn=false;
void ContribEnd(int);
static bool exerciseActualDataChanged=false;
void ProductionDataChanged();
void DataChanged(){assert(!worker);++dataChanges;if(exerciseActualDataChanged){ ProductionDataChanged(); }}
void WaitFunc(){}
static int activations=0;
void ProductionActivateFunc();
void ActivateFunc(){++activations;if(exerciseActualDataChanged)ProductionActivateFunc();}
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
   assert(!worker);JsonDocument doc;if(deserializeJson(doc,body.c_str())){ return false; }
   if(!strcmp(kind,"Loop")){ return shift_machine.set(doc); }
   if(!strcmp(kind,"ReceiveMine")){ return my.set(doc); }
   return tag.set(doc);
 }
 void FirmwareUpdate(const char*,const char*){++otaChecks;otaTrace.emplace_back("legacy");}
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
static bool serverWireBypass=false;
bool WireServerCompleted(){assert(!worker);return serverWireBypass;}
static GeneratorWireState wireState;
static void (*duringWireSnapshot)()=nullptr;
GeneratorWireState WireSnapshot(){auto value=wireState;if(duringWireSnapshot){auto hook=duringWireSnapshot;duringWireSnapshot=nullptr;hook();}return value;}
static Has2BoundedTransport::Result nextReply;
static void (*duringRequest)()=nullptr;
static std::vector<std::string> paths;
namespace Has2BoundedTransport {
Result get(const char*,uint16_t,const char *path,const Options &options){
 assert(worker&&depth==0);assert(options.deadlineMs==2000);++requestCalls;paths.emplace_back(path);
 if(strstr(path,"GeneratorWires")){assert(options.bodyDeadlineMs==1000&&options.maxBodyBytes==1024&&options.requireContentLength);}
 if(options.progressHook){ options.progressHook(); }
 if(duringRequest){auto hook=duringRequest;duringRequest=nullptr;hook();}
 now+=nextReply.timings.totalMs;
 return nextReply;
}
}
'''
application_stubs=r'''
namespace Has1BleBeacon {
 void setDeviceName(const char*){}
 void suspend(){otaTrace.emplace_back("ble-off");}
 bool releaseMemory(){otaTrace.emplace_back("ble-release");return true;}
}
static int gameTimerDeletes=0;
struct Timer {void deleteTimer(int){++gameTimerDeletes;} int setInterval(unsigned,void(*)()){return 1;} bool isEnabled(int){return true;}} GameTimer,BlinkTimer;
int gameTimerId=1,blinkTimerId=2,starterDecreaseAmount=1;
bool starterTaggerActive=false;
void (*ptrRfidMode)()=WaitFunc;
const int BLUE=0,RED=1,YELLOW=2,GREEN=3,STARTER=0,DEVICESTATE=1,CIRCUIT=2;
int color[4]={0,1,2,3};
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
void TaggerEnter(){ptrCurrentMode=TaggerRfidLoop;}
void BatteryFinish();void StartFinish();int StarterGaugeCnt();
bool batteryFinishDone=false,batteryFinishAudioPlayed=false,starterRfidNeedsValidation=false;
int displayedGaugeNeoCnt=-1,gameTimerCnt=0;unsigned gameTime=400;
void GameTimerFunc(){}
bool WireReadyForCompletion(){return serverWireBypass||wireState.qualified(now);}
void WireAcceptServerCompletion(){serverWireBypass=true;}
void BatteryPackSend(){}
const int MAINPN532=0;
struct Nfc{void SAMConfig(){}} nfc[1];
void RfidLoopMain(){}
void BlinkTimerStart(int,int){}
void Mp3BatteryStart(){}
bool Mp3BatteryFinished(){return true;}
void EncoderNeopixelOn(int){}

'''
tests=r'''
static void reply(const char *json,int status=200){nextReply=Has2BoundedTransport::Result{};nextReply.status=status;nextReply.payload=json;nextReply.timings.totalMs=100;}
// Transport failure at a given stage. Connect means no request byte was written;
// Header means the GET was written and the response was lost (uncertain).
static void failAt(Has2BoundedTransport::Phase phase,Has2BoundedTransport::Error error=Has2BoundedTransport::Error::Deadline){
 nextReply=Has2BoundedTransport::Result{};nextReply.error=error;nextReply.phase=phase;nextReply.timings.totalMs=2000;}
static int sendCount(){int sends=0;for(const auto &path:paths){if(path.find("request=Send")!=std::string::npos){++sends;}}return sends;}
static int queuedCompletions(){int n=0;for(const auto &slot:networkSlots){if(slot.occupied&&slot.request.kind==NetworkKind::Completion){++n;}}return n;}
static void workerTurn(){worker=true;try{NetworkWorker(nullptr);}catch(const Yield&){}worker=false;}
static void mainTurn(){GeneratorNetworkLoop();}
static int used(){
 int count=0;
 for(const auto&s:networkSlots) {
  if(s.occupied) {++count;}
 }
 return count;
}
static void reset(){
 networkRecoveryPolicy=GeneratorOfflineRecovery::Policy{};networkRecoveryHistory=GeneratorOfflineRecovery::Retained{};
 GeneratorOfflineRecovery::seal(networkRecoveryHistory);networkRecoveryFence=false;networkRecoverySalt=networkRecoveryLastLog=0;networkRecoveryBlockedLogged=false;
 reconnectOnRecheck=false;wifiStatusCalls=0;
 now=100;serverWireBypass=false;online=true;worker=false;failWatchdog=false;requestCalls=restarts=dataChanges=otaChecks=0;depth=0;
 batteryFinishDone=batteryFinishAudioPlayed=starterRfidNeedsValidation=false;displayedGaugeNeoCnt=-1;
 exerciseActualDataChanged=false;receiveMineOn=false;ptrCurrentMode=WaitFunc;cur.clear();gameTimerDeletes=0;
 paths.clear();diagnostics.clear();my.clear();tag.clear();shift_machine.clear();
 for(auto &s:networkSlots){ s=NetworkSlot{}; }
 networkTask=(void*)1;networkGeneration=1;networkNextId=1;networkMailboxReady=false;networkMailboxSlot=-1;networkMailboxCancelled=false;
 networkMailbox=Has2BoundedTransport::Result{};
 networkPollPending=networkPollEvent=networkPollSuccess=networkPollAwaitingRefresh=false;networkUrgentRun=0;networkWorkerActiveId=0;networkScanPending=networkDriverFault=false;networkRefreshPending=networkRefreshInFlight=networkResetCyclePending=false;activations=0;networkBootReady=true;
 networkVersionSent=true;networkWatchdogPending=networkWatchdogRequested=false;networkWasConnected=true;
 networkMaintenance=networkMaintenanceLegacy=false;networkRefreshAt=0;networkIdentity[0]=networkGame[0]=0;
 otaTrace.clear();blackboxUploadIdle=true;duringOta=nullptr;networkOtaRestartPending=false;NetworkOtaAttemptsSet(0);
 otaRestartLogs.clear();resetReason=ESP_RST_POWERON;guardArms=guardStops=0;guardArmed=false;
 pendingContributionCount=0;starterContribUser="";
 networkCompletionGeneration=0;networkCompletionToken=1;networkCompletion[0]=0;networkCompletionSent=networkCompletionVerify=networkCompletionConfirmed=networkCompletionSuperseded=networkCompletionDispatched=false;
 networkCompletionAttempts=0;NetworkCompletionResendClear();networkCompletionWireCount=-1;networkCompletionWireGeneration=0;networkCompletionWireBypass=false;
 duringWireSnapshot=nullptr;
 networkCompletionSince=networkCompletionWarningAt=0;
 GeneratorNetworkForgetTag();wireHttpPending=wireHttpDone=false;wireHttpRequestId=0;
 wireState.reset();for(int i=0;i<=10;++i){ wireState.sampleMask(i*100,0); }
 my["device_name"]="TG";my["game_state"]="activate";my["device_state"]="activate";
 strcpy(networkIdentity,"TG");strcpy(networkGame,"activate");
 now=1100;reply("{}");
}
static void pass(const char *text){printf("PASS %s\n",text);}
int main(){
 reset();assert(GeneratorNetworkRequestPoll());for(int i=0;i<1000;++i) {assert(GeneratorNetworkRequestPoll());}
 assert(used()==1&&requestCalls==0);reply("{\"shift_machine\":0,\"watchdog\":0}");workerTurn();assert(requestCalls==1&&dataChanges==0);
 mainTurn();bool ok=false;assert(GeneratorNetworkTakePollResult(ok)&&ok&&!GeneratorNetworkPollBusy());
 pass("poll coalescing and worker/main ownership; no transport on enqueue");

 reset();networkRefreshPending=true;assert(NetworkQueueRefresh());assert(GeneratorNetworkRequestPoll());
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"activate\"}");workerTurn();mainTurn();
 assert(networkPollPending&&!networkPollEvent);reply("{\"shift_machine\":0,\"watchdog\":0}");workerTurn();mainTurn();
 assert(!networkPollPending&&GeneratorNetworkTakePollResult(ok)&&ok);
 pass("independent refresh result cannot complete a still-queued poll");

 reset();for(int i=0;i<6;++i) {assert(GeneratorNetworkSend("TG","crash_log","x"));}
 assert(GeneratorNetworkRequestPoll());
 for(int i=0;i<4;++i){reply("{\"shift_machine\":0,\"watchdog\":0}");workerTurn();mainTurn();}
 assert(paths.size()==4&&paths[3].find("request=Loop")!=std::string::npos);
 pass("heartbeat is served after at most three other completed operations");

 reset();online=false;for(int i=0;i<6;++i){ assert(GeneratorNetworkSend("TG","crash_log","x")); }
 assert(!GeneratorNetworkSend("TG","crash_log","overflow"));assert(GeneratorNetworkRequestPoll());assert(used()==7);
 for(int i=0;i<500;++i) {workerTurn();}
 assert(used()==7&&requestCalls==0);
 online=true;reply("{}");workerTurn();assert(networkMailboxReady&&requestCalls==0);mainTurn();workerTurn();assert(networkMailboxReady&&requestCalls==1);for(int i=0;i<20;++i) {workerTurn();}
 assert(requestCalls==1);
 pass("bounded optional queue reserves heartbeat capacity; offline and unread result cannot spawn work");

 reset();reply("{\"role\":\"player\"}");assert(!GeneratorNetworkTag("G1P1"));workerTurn();assert(tag.isNull());
 GeneratorNetworkForgetTag();mainTurn();assert(tag.isNull());
 assert(!GeneratorNetworkTag("G1P2"));nextReply.error=Has2BoundedTransport::Error::Deadline;workerTurn();mainTurn();
 int calls=requestCalls;for(int i=0;i<100;++i) {assert(!GeneratorNetworkTag("G1P2"));}
 assert(requestCalls==calls&&used()==0);
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
 for(int i=11;i<22;++i){ wireState.sampleMask(i*100,1); }
 assert(WireHttpRequest(true,"TG",1,"11111111-1111-1111-1111-111111111111",1,snapshot)==GeneratorWireResult::Pending);
 workerTurn();assert(requestCalls==1&&paths[0].find("value=1")!=std::string::npos);
 pass("queued wire count coalesces to the latest qualified count");

 reset();
 assert(GeneratorNetworkContribution("G1P1",2));

 GeneratorNetworkInvalidateGame();workerTurn();assert(requestCalls==0);mainTurn();
 pass("game/epoch invalidation rejects queued mutation before send");

 reset();
 for(int i=0;i<6;++i){ assert(GeneratorNetworkSend("TG","crash_log","x")); }
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

 reset();blackboxUploadIdle=false;GeneratorNetworkRequestMaintenance();
 for(int i=0;i<10;++i){now+=1000;mainTurn();}
 assert(networkMaintenance&&otaChecks==0&&otaTrace.empty());
 blackboxUploadIdle=true;mainTurn();assert(otaChecks==1&&networkMaintenance&&networkOtaRestartPending&&restarts==0);
 assert((otaTrace==std::vector<std::string>{"pause","ble-off","ble-release","ota","unpause"}));
 {bool logged=false;for(const auto &line:diagnostics){if(line.find("OTA memory")!=std::string::npos){logged=true;}}assert(logged);}
 assert(NetworkOtaAttempts()==1&&guardArms==1&&guardStops==1&&!guardArmed);
 mainTurn();assert(restarts==1&&NetworkOtaAttempts()==1&&otaChecks==1);
 assert((otaRestartLogs==std::vector<uint32_t>{1}));
 pass("GitHub OTA waits for the MQTT session, releases BLE memory, then restarts when it returns");

 reset();duringOta=[](){assert(NetworkOtaAttempts()==1);};GeneratorNetworkRequestMaintenance();mainTurn();
 assert(otaChecks==1);
 pass("the attempt is counted before the check, so a crash inside it still counts");

 reset();duringOta=[](){assert(GeneratorNetworkSend("TG","device_state","setting"));};
 GeneratorNetworkRequestMaintenance();mainTurn();assert(otaChecks==1&&used()==1);
 mainTurn();assert(restarts==0);
 reply("{}");workerTurn();assert(paths.size()==1&&paths[0].find("value=setting")!=std::string::npos);
 mainTurn();assert(restarts==1);
 pass("restart after OTA waits for the callback's device_state report to leave");

 reset();duringOta=[](){assert(GeneratorNetworkSend("TG","device_state","setting"));};
 GeneratorNetworkRequestMaintenance();mainTurn();online=false;
 now+=9999;mainTurn();assert(restarts==0);
 now+=1;mainTurn();assert(restarts==1&&requestCalls==0);
 pass("an undeliverable OTA report delays the restart by at most ten seconds");

 reset();
 for(uint32_t i=1;i<=3;++i) {
  GeneratorNetworkRequestMaintenance();mainTurn();assert(otaChecks==int(i)&&NetworkOtaAttempts()==i);
  mainTurn();assert(restarts==int(i)&&!networkMaintenance);
 }
 GeneratorNetworkRequestMaintenance();assert(!networkMaintenance);
 mainTurn();assert(otaChecks==3&&restarts==3);
 GeneratorNetworkRequestMaintenance(true);assert(networkMaintenance&&networkMaintenanceLegacy);
 pass("three GitHub OTA attempts stop further GitHub attempts; local update is still allowed");

 reset();
 const int kept[]={ESP_RST_SW,ESP_RST_PANIC,ESP_RST_INT_WDT,ESP_RST_TASK_WDT,ESP_RST_WDT};
 for(int reason:kept){NetworkOtaAttemptsSet(2);resetReason=reason;networkTask=nullptr;GeneratorNetworkBegin();assert(NetworkOtaAttempts()==2);}
 NetworkOtaAttemptsSet(2);resetReason=ESP_RST_POWERON;networkTask=nullptr;GeneratorNetworkBegin();assert(NetworkOtaAttempts()==0);
 pass("the attempt count survives software, panic and watchdog resets but not power-on");

 reset();NetworkOtaAttemptsSet(2);networkRefreshPending=true;assert(NetworkQueueRefresh());
 reply("{\"device_name\":\"TG\",\"game_state\":\"setting\",\"device_state\":\"github\"}");workerTurn();mainTurn();
 assert(NetworkOtaAttempts()==2);
 networkRefreshPending=true;assert(NetworkQueueRefresh());
 reply("{\"device_name\":\"TG\",\"game_state\":\"setting\",\"device_state\":\"setting\"}");workerTurn();mainTurn();
 assert(NetworkOtaAttempts()==0);
 pass("any device_state other than github clears the GitHub OTA attempt count");

 reset();blackboxUploadIdle=false;GeneratorNetworkRequestMaintenance();
 now+=14999;mainTurn();assert(otaChecks==0);
 now+=1;mainTurn();assert(otaChecks==1&&networkOtaRestartPending);
 pass("a stuck MQTT stop delays GitHub OTA by at most 15 seconds");

 reset();blackboxUploadIdle=false;GeneratorNetworkRequestMaintenance(true);mainTurn();
 assert(otaChecks==1&&!networkMaintenance&&restarts==0);
 assert((otaTrace==std::vector<std::string>{"pause","legacy","unpause"}));
 pass("local HTTP update keeps its path: no MQTT wait and no BLE stop");

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
 assert(!GeneratorNetworkComplete("battery_max"));assert(used()==1);
 failAt(Has2BoundedTransport::Phase::Header);workerTurn();mainTurn();
 assert(used()==1); // Main loop schedules verification even without another game callback.
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"battery_max\"}");workerTurn();mainTurn();
 assert(GeneratorNetworkComplete("battery_max"));assert(requestCalls==2);
 pass("legacy completion verifies uncertain mutation and never blindly repeats Send");
 reset();my["device_state"]="battery_max";my["left_generator"]="3";cur.set(my);
 ptrCurrentMode=StarterActivate;exerciseActualDataChanged=true;encoderValue=28;tag["role"]="player";
 assert(!GeneratorNetworkComplete("repaired"));
 // Remove the card and decay the gauge before Send returns. No StartFinish reentry.
 tag.clear();encoderValue=0;failAt(Has2BoundedTransport::Phase::Header);workerTurn();mainTurn();assert(used()==1);
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"repaired\",\"left_generator\":\"3\"}");
 workerTurn();mainTurn();assert(ptrCurrentMode==WaitFunc&&gameTimerDeletes>=2&&!receiveMineOn);
 assert(!strcmp(cur["device_state"].as<const char*>(),"repaired"));assert(requestCalls==2);
 pass("actual DataChanged completes repaired after card removal and gauge decay without StartFinish reentry");


 reset();assert(GeneratorNetworkContribution("G1P1",3));reply("{}");workerTurn();mainTurn();
 assert(requestCalls==1&&paths[0].find("request=Situation")!=std::string::npos&&paths[0].find("G1P1%3A3")!=std::string::npos);
 for(int i=0;i<20;++i){mainTurn();workerTurn();}assert(requestCalls==1);
 pass("default build sends legacy contribution once without a background retry");

 reset();serverWireBypass=true;assert(!GeneratorNetworkComplete("battery_max"));reply("{\"success\":true}");workerTurn();mainTurn();
 assert(!networkCompletionConfirmed);reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"battery_max\"}");workerTurn();mainTurn();
 assert(GeneratorNetworkComplete("battery_max"));assert(!GeneratorNetworkComplete("repaired"));reply("{}");workerTurn();mainTurn();
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"repaired\"}");workerTurn();mainTurn();
 assert(GeneratorNetworkComplete("repaired")&&requestCalls==4);
 pass("normal legacy battery then repair each sends once and confirms by fresh snapshot");

 reset();serverWireBypass=true;assert(!GeneratorNetworkComplete("repaired"));failAt(Has2BoundedTransport::Phase::Header);workerTurn();mainTurn();
 for(int i=0;i<18;++i){reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"activate\"}");workerTurn();mainTurn();now+=2100;mainTurn();}
 int sends=sendCount();
 bool warning=false;for(const auto &line:diagnostics){if(line.find("staff recovery required")!=std::string::npos){warning=true;}}
 assert(sends==1&&!networkCompletionConfirmed&&warning);
 pass("unapplied uncertain completion is read-only retried with bounded staff-recovery warning");
 for(auto phase:{Has2BoundedTransport::Phase::Write,Has2BoundedTransport::Phase::Header,Has2BoundedTransport::Phase::Body}) {
  reset();serverWireBypass=true;assert(!GeneratorNetworkComplete("battery_max"));failAt(phase);workerTurn();mainTurn();
  for(int i=0;i<6;++i){now+=2100;mainTurn();reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"activate\"}");workerTurn();mainTurn();}
  assert(sendCount()==1&&!networkCompletionResendPending);
 }
 pass("a failure after any request byte stays uncertain and is never resent");

 // Connect-phase failure: nothing reached the server. The resend is armed only by a
 // successful verify that still shows the start state, then confirmed by snapshot.
 reset();serverWireBypass=true;assert(!GeneratorNetworkComplete("battery_max"));uint32_t firstToken=networkCompletionToken;
 failAt(Has2BoundedTransport::Phase::Connect);workerTurn();mainTurn();
 assert(networkCompletionResendPending&&networkCompletionSent&&!networkCompletionDispatched&&queuedCompletions()==0);
 failAt(Has2BoundedTransport::Phase::Connect);workerTurn();mainTurn(); // verify also fails: no resend without fresh evidence
 now+=2100;mainTurn();assert(queuedCompletions()==0&&networkCompletionResendPending);
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"activate\"}");workerTurn();mainTurn(); // verify ok, still start state
 assert(queuedCompletions()==1&&!networkCompletionResendPending&&networkCompletionResendId!=0);
 bool sameToken=true;for(const auto &slot:networkSlots){if(slot.occupied&&slot.request.kind==NetworkKind::Completion&&slot.request.token!=firstToken){sameToken=false;}}
 assert(sameToken&&networkCompletionToken==firstToken);
 reply("{}");workerTurn();mainTurn();assert(sendCount()==2&&networkCompletionDispatched&&networkCompletionResendId==0);
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"battery_max\"}");
 for(int i=0;i<4&&!networkCompletionConfirmed;++i){now+=2100;mainTurn();workerTurn();mainTurn();}
 assert(networkCompletionConfirmed&&GeneratorNetworkComplete("battery_max")&&sendCount()==2);
 pass("connect-phase failure is resent once a fresh verify shows the start state, then confirmed by snapshot");

 reset();serverWireBypass=true;assert(!GeneratorNetworkComplete("battery_max"));
 for(int attempt=1;attempt<=3;++attempt){
  failAt(Has2BoundedTransport::Phase::Connect);workerTurn();mainTurn(); // send attempt fails at connect; verify queued
  reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"activate\"}");workerTurn();mainTurn(); // verify ok
  if(attempt<3){ assert(queuedCompletions()==1&&!networkCompletionResendPending); }
  else { assert(queuedCompletions()==0&&!networkCompletionResendPending); }
 }
 now+=2100;mainTurn();bool verifyQueued=false;for(const auto &slot:networkSlots){if(slot.occupied&&slot.request.kind==NetworkKind::Verify){verifyQueued=true;}}
 now+=30000;mainTurn();
 bool exhausted=false,budgetWarning=false;for(const auto &line:diagnostics){if(line.find("resend budget exhausted")!=std::string::npos){exhausted=true;}if(line.find("staff recovery required")!=std::string::npos){budgetWarning=true;}}
 assert(sendCount()==3&&exhausted&&budgetWarning&&verifyQueued&&!networkCompletionConfirmed&&!networkCompletionResendPending&&networkCompletionSent);
 pass("connect-phase resends are bounded to three total attempts, then read-only verification continues");

 reset();serverWireBypass=true;assert(!GeneratorNetworkComplete("battery_max"));failAt(Has2BoundedTransport::Phase::Connect);workerTurn();mainTurn();
 assert(networkCompletionResendPending&&!networkCompletionDispatched);
 JsonDocument staff;assert(!deserializeJson(staff,"{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"battery_max\"}"));
 assert(!NetworkOwnBatteryEcho(staff)); // nothing was delivered, so this cannot be the device's own echo
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"battery_max\"}");assert(NetworkApplySnapshot(nextReply,false));
 assert(networkCompletionConfirmed&&!networkCompletionResendPending);
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"battery_max\"}");workerTurn();mainTurn();now+=5000;mainTurn(); // pending verify completes; no resend follows
 assert(queuedCompletions()==0&&sendCount()==1&&GeneratorNetworkComplete("battery_max"));
 pass("staff override arriving before any resend confirms the stage and is not mistaken for the device's own echo");

 reset();serverWireBypass=true;assert(!GeneratorNetworkComplete("battery_max"));failAt(Has2BoundedTransport::Phase::Connect);workerTurn();mainTurn();
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"tagger\"}");assert(NetworkApplySnapshot(nextReply,false));
 assert(!networkCompletionResendPending);
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"activate\"}");assert(NetworkApplySnapshot(nextReply,false)); // back to the start value
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"activate\"}");workerTurn();mainTurn();
 now+=2100;mainTurn();reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"activate\"}");workerTurn();mainTurn();
 assert(queuedCompletions()==0&&sendCount()==1&&!networkCompletionResendPending);
 pass("a brief authoritative state change away from the start state permanently drops the resend");

 reset();assert(!GeneratorNetworkComplete("battery_max"));failAt(Has2BoundedTransport::Phase::Connect);workerTurn();mainTurn();
 assert(networkCompletionResendPending);for(int i=1;i<=10;++i){wireState.sampleMask(now+i*100,1);}
 now+=1100;reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"activate\"}");workerTurn();mainTurn(); // verify ok arms the resend with the original expectations
 assert(queuedCompletions()==1);workerTurn();mainTurn(); // dispatch guard rejects the changed wires
 assert(requestCalls==2&&sendCount()==1&&queuedCompletions()==0&&!networkCompletionSent&&!networkCompletionResendPending);
 pass("resend keeps the original wire expectations so a wire change is rejected at dispatch");

 reset();serverWireBypass=true;assert(!GeneratorNetworkComplete("battery_max"));failAt(Has2BoundedTransport::Phase::Connect);workerTurn();mainTurn();
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"activate\"}");workerTurn();mainTurn();assert(queuedCompletions()==1);uint32_t keptToken=networkCompletionToken;
 online=false;mainTurn();workerTurn();mainTurn(); // offline: the queued resend is withdrawn before dispatch
 assert(queuedCompletions()==0&&networkCompletionResendPending&&networkCompletionSent&&networkCompletionToken==keptToken&&requestCalls==2);
 online=true;now+=2100;mainTurn(); // reconnect queues poll, refresh and the due verify
 for(int i=0;i<3;++i){reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"activate\"}");workerTurn();mainTurn();} // reads only; the fresh verify re-arms
 assert(queuedCompletions()==1&&sendCount()==1);reply("{}");workerTurn();mainTurn();assert(sendCount()==2);
 pass("a resend queued when WiFi drops is withdrawn and re-armed only by a fresh verify after reconnect");

 reset();serverWireBypass=true;assert(!GeneratorNetworkComplete("battery_max"));failAt(Has2BoundedTransport::Phase::Connect);workerTurn();mainTurn();
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"activate\"}");workerTurn();mainTurn();assert(queuedCompletions()==1);
 now+=5100;mainTurn();workerTurn();mainTurn(); // not dispatched within the queue lifetime
 assert(queuedCompletions()==0&&networkCompletionResendPending&&networkCompletionSent);
 pass("a resend that waits too long in the queue is withdrawn rather than dispatched against stale evidence");

 reset();serverWireBypass=true;assert(!GeneratorNetworkComplete("battery_max"));
 failAt(Has2BoundedTransport::Phase::Connect,Has2BoundedTransport::Error::InvalidRequest);workerTurn();mainTurn();
 assert(!networkCompletionResendPending&&networkCompletionSent&&sendCount()==1);
 pass("an invalid request is not treated as an undelivered connect failure");

 reset();serverWireBypass=true;assert(!GeneratorNetworkComplete("battery_max"));failAt(Has2BoundedTransport::Phase::Connect);workerTurn();mainTurn();
 GeneratorNetworkInvalidateGame();now+=2100;mainTurn();workerTurn();mainTurn();
 for(int i=0;i<10;++i){now+=2100;mainTurn();workerTurn();mainTurn();}
 assert(requestCalls==1&&sendCount()==1);
 pass("game change after an undelivered send cannot trigger a resend for the old game");

 reset();serverWireBypass=true;assert(!GeneratorNetworkComplete("battery_max")&&networkCompletionSent);
 failAt(Has2BoundedTransport::Phase::Connect);workerTurn();mainTurn();assert(networkCompletionResendPending&&used()==1); // verify queued
 for(int i=0;i<6;++i){assert(NetworkSubmit(NetworkKind::Tag,"/filler",false,0,networkTagToken,nullptr));}
 assert(GeneratorNetworkRequestPoll()&&used()==8);
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"activate\"}");workerTurn();mainTurn(); // verify ok but the queue is full
 assert(networkCompletionResendPending&&queuedCompletions()==0);
 GeneratorNetworkForgetTag();now+=2100;mainTurn(); // next verify is queued once capacity returns
 for(int i=0;i<2;++i){reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"activate\"}");workerTurn();mainTurn();} // poll and verify, either order
 assert(!networkCompletionResendPending&&queuedCompletions()==1);
 pass("a full queue defers the resend until the next fresh verify instead of dropping or duplicating it");

 reset();assert(!GeneratorNetworkComplete("battery_max"));for(int i=1;i<=10;++i){wireState.sampleMask(now+i*100,1);}workerTurn();mainTurn();
 assert(requestCalls==0&&!networkCompletionSent);
 pass("wire change cancels queued completion before transmission");
 reset();serverWireBypass=true;wireState.reset();assert(!GeneratorNetworkComplete("repaired"));workerTurn();assert(requestCalls==1);
 pass("staff forced completion retains explicit physical gate bypass");

 reset();assert(!GeneratorNetworkComplete("battery_max"));duringWireSnapshot=[](){++networkGeneration;};workerTurn();mainTurn();
 assert(requestCalls==0);
 pass("game cancellation between preflight and dispatch wins under the final dispatch lock");
 reset();serverWireBypass=true;assert(!GeneratorNetworkComplete("repaired"));duringRequest=[](){++networkGeneration;};workerTurn();mainTurn();
 assert(requestCalls==1&&!networkCompletionConfirmed);for(int i=0;i<20;++i){mainTurn();workerTurn();}assert(requestCalls==1);
 pass("game change after dispatch discards outcome and cannot trigger automatic mutation retry");

 reset();serverWireBypass=true;assert(!GeneratorNetworkComplete("battery_max"));reply("{}");workerTurn();mainTurn();
 NetworkRequest oldVerify={};for(const auto &slot:networkSlots){if(slot.occupied&&slot.request.kind==NetworkKind::Verify){oldVerify=slot.request;}}
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"starter_finish\"}");assert(NetworkApplySnapshot(nextReply,false));
 assert(!GeneratorNetworkComplete("repaired"));uint32_t token=networkCompletionToken;
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"battery_max\"}");NetworkCompletionVerifyResult(oldVerify,nextReply,false);
 assert(networkCompletionToken==token&&!strcmp(networkCompletion,"repaired")&&!strcmp(my["device_state"]|"","starter_finish"));
 pass("authoritative advancement releases prior stage and late old verify cannot overwrite the next operation");

 reset();assert(!GeneratorNetworkComplete("battery_max"));reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"repaired_all\"}");
 assert(NetworkApplySnapshot(nextReply,false));workerTurn();mainTurn();assert(requestCalls==0);
 pass("authoritative terminal state cancels an obsolete queued completion");
 reset();serverWireBypass=true;my["device_state"]="battery_max";assert(!GeneratorNetworkComplete("repaired"));
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"tagger\"}");assert(NetworkApplySnapshot(nextReply,false));workerTurn();mainTurn();assert(requestCalls==0&&!networkCompletionSent);
 my["device_state"]="starter_finish";assert(!GeneratorNetworkComplete("repaired"));
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"starter_finish\"}");assert(NetworkApplySnapshot(nextReply,false));workerTurn();assert(requestCalls==1);
 pass("known unsent cancellation recaptures current state before a valid retry");


 reset();serverWireBypass=true;assert(!GeneratorNetworkComplete("battery_max"));reply("{}");workerTurn();mainTurn();
 reply("{\"device_name\":\"WRONG\",\"game_state\":\"activate\",\"device_state\":\"battery_max\"}");workerTurn();mainTurn();assert(!networkCompletionConfirmed&&!strcmp(my["device_name"]|"","TG"));
 now+=2100;mainTurn();reply("{\"device_name\":\"TG\",\"device_state\":\"battery_max\"}");workerTurn();mainTurn();assert(!networkCompletionConfirmed);
 pass("wrong-device and malformed completion snapshots cannot confirm or replace current state");

 reset();for(int i=0;i<7;++i){assert(NetworkSubmit(NetworkKind::Tag,"/filler",false,0,networkTagToken,nullptr));}
 assert(!GeneratorNetworkComplete("battery_max")&&!networkCompletionSent);reply("{}");workerTurn();mainTurn();
 assert(!GeneratorNetworkComplete("battery_max")&&networkCompletionSent);for(int i=0;i<100;++i){assert(!GeneratorNetworkComplete("battery_max"));}
 int completions=0;for(const auto &slot:networkSlots){if(slot.occupied&&slot.request.kind==NetworkKind::Completion){++completions;}}
 assert(completions==1);
 pass("queue-full rejection may retry enqueue but never duplicates an accepted completion");

 reset();my["left_generator"]="3";cur.set(my);exerciseActualDataChanged=true;assert(!GeneratorNetworkComplete("battery_max"));reply("{}");workerTurn();mainTurn();
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"battery_max\",\"left_generator\":\"3\"}");
 assert(NetworkApplySnapshot(nextReply,false));assert(activations==0&&networkCompletionConfirmed&&!receiveMineOn);
 pass("ordinary refresh of own dispatched battery echo cannot masquerade as staff override");
 reset();my["device_state"]="battery_max";my["left_generator"]="3";cur.set(my);exerciseActualDataChanged=true;ptrCurrentMode=StarterActivate;receiveMineOn=true;
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"repaired\",\"left_generator\":\"3\"}");
 assert(NetworkApplySnapshot(nextReply,false));assert(ptrCurrentMode==WaitFunc&&!receiveMineOn&&gameTimerDeletes>=2);
 pass("ordinary repaired refresh cannot be swallowed by a stale battery echo guard");

 reset();my["left_generator"]="3";cur.set(my);exerciseActualDataChanged=true;ptrCurrentMode=BatteryFinish;
 BatteryFinish();assert(!batteryFinishDone&&networkCompletionSent);
 reply("{}");workerTurn();mainTurn();
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"battery_max\",\"left_generator\":\"3\"}");workerTurn();mainTurn();
 BatteryFinish();assert(batteryFinishDone&&ptrCurrentMode==StarterActivate&&!serverWireBypass);
 StartFinish();reply("{}");workerTurn();mainTurn();
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"repaired\",\"left_generator\":\"3\"}");workerTurn();mainTurn();
 assert(ptrCurrentMode==WaitFunc&&networkCompletionConfirmed&&requestCalls==4&&!serverWireBypass);
 pass("actual BatteryFinish and StartFinish complete legacy gameplay without granting staff wire bypass");

 reset();my["left_generator"]="3";cur.set(my);exerciseActualDataChanged=true;
 assert(!GeneratorNetworkComplete("battery_max"));reply("{}");workerTurn();mainTurn();
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"battery_max\",\"left_generator\":\"3\"}");workerTurn();mainTurn();batteryFinishDone=true;
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"tagger\",\"left_generator\":\"3\"}");assert(NetworkApplySnapshot(nextReply,false));assert(ptrCurrentMode==TaggerRfidLoop);
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"battery_max\",\"left_generator\":\"3\"}");assert(NetworkApplySnapshot(nextReply,false));
 assert(ptrCurrentMode==BatteryFinish&&!serverWireBypass);BatteryFinish();assert(ptrCurrentMode==StarterActivate);
 pass("confirmed local battery transaction cannot swallow later authoritative tagger return");

 // Prior association is sampled from the current link, not boot-ready game state.
 reset();mainTurn();assert(networkRecoveryPolicy.hadConnection);
 online=false;mainTurn();const uint32_t disconnectedAt=now;
 now=disconnectedAt+29999;mainTurn();assert(restarts==0);
 now=disconnectedAt+30000;mainTurn();assert(restarts==1&&networkRecoveryHistory.outageMs==30000);
 pass("previously associated Wi-Fi recovers after 30 seconds of continuous outage");

 reset();mainTurn();online=false;mainTurn();now+=29000;mainTurn();assert(restarts==0);
 online=true;mainTurn();online=false;mainTurn();const uint32_t secondDropAt=now;
 now=secondDropAt+29999;mainTurn();assert(restarts==0);
 now=secondDropAt+30000;mainTurn();assert(restarts==1);
 pass("brief reconnection restarts the continuous-outage timer while retaining per-boot prior association");

 reset();GeneratorOfflineRecovery::requested(networkRecoveryHistory,30000);
 assert(GeneratorOfflineRecovery::boot(networkRecoveryHistory,true));mainTurn();
 online=false;mainTurn();const uint32_t retryAt=now;
 now=retryAt+30000;mainTurn();assert(restarts==0);
 now=retryAt+299999;mainTurn();assert(restarts==0);
 now=retryAt+300000;mainTurn();assert(restarts==1&&networkRecoveryHistory.attempts==2);
 pass("repeated recovery boot keeps storm backoff even after a brief online observation");

 // Overall outage recovery: failed association returns normally, so neither
 // task watchdog need fire; the independent outage timer must still act.
 reset();online=false;mainTurn();
 for(int i=0;i<17;++i){now+=10000;workerTurn();mainTurn();assert(restarts==0);}
 now+=11000;workerTurn();mainTurn();assert(restarts==1&&networkRecoveryFence&&networkRecoveryHistory.pending==1);
 assert(networkRecoveryHistory.attempts==1&&networkRecoveryHistory.outageMs>=180000);
 pass("healthy worker returning offline still triggers a fenced restart after the sustained outage deadline");

 reset();online=true;reply("{}",503);
 for(int i=0;i<30;++i){GeneratorNetworkRequestPoll();workerTurn();mainTurn();now+=60000;}
 assert(restarts==0);
 pass("long HTTP server errors while Wi-Fi remains associated never trigger outage restarts");

 reset();online=false;assert(!NetworkOfflineRecoveryService());now+=180000;
 networkMaintenance=true;assert(!NetworkOfflineRecoveryService()&&restarts==0&&!networkRecoveryFence);
 networkMaintenance=false;networkDriverFault=true;assert(!NetworkOfflineRecoveryService()&&restarts==0);
 networkDriverFault=false;networkSlots[0].occupied=true;networkSlots[0].active=true;
 assert(!NetworkOfflineRecoveryService()&&restarts==0&&!networkRecoveryFence);
 networkSlots[0].occupied=false;networkMailboxReady=true;
 assert(!NetworkOfflineRecoveryService()&&restarts==0);
 networkMailboxReady=false;assert(NetworkOfflineRecoveryService()&&restarts==1);
 pass("OTA, failed worker, active request and unread mailbox defer recovery without taking the dispatch fence");

 reset();serverWireBypass=true;assert(!GeneratorNetworkComplete("repaired"));
 failAt(Has2BoundedTransport::Phase::Header);workerTurn();mainTurn();
 assert(networkCompletionSent&&networkCompletionDispatched&&!networkCompletionConfirmed);
 online=false;mainTurn();now+=180000;workerTurn();mainTurn();
 assert(restarts==0&&!networkRecoveryFence&&sendCount()==1);
 now+=3600000;workerTurn();mainTurn();assert(restarts==0&&sendCount()==1);
 pass("possibly delivered completion retains read-only confirmation even through a long outage; no restart/replay");

 reset();online=false;assert(!NetworkOfflineRecoveryService());now+=180000;
 reconnectOnRecheck=true;wifiStatusCalls=0;
 assert(!NetworkOfflineRecoveryService()&&online&&restarts==0&&!networkRecoveryFence&&networkRecoveryPolicy.hadConnection);
 pass("live association recheck after fencing prevents a stale-offline restart");

 reset();assert(GeneratorNetworkRequestPoll());networkRecoveryFence=true;workerTurn();
 assert(requestCalls==0);assert(!GeneratorNetworkContribution("G1P1",1));
 pass("restart fence prevents worker selection and new enqueue");

 reset();serverWireBypass=false;assert(!GeneratorNetworkComplete("battery_max"));
 duringWireSnapshot=[](){networkRecoveryFence=true;};workerTurn();mainTurn();
 assert(requestCalls==0);
 pass("restart fence published after preflight still wins at the final dispatch boundary");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='generator-network-test-') as tmp:
 p=Path(tmp);src=p/'test.cpp';src.write_text(prefix+'\n'+application_stubs+'\n'+actual_datachanged+'\n'+actual_activate+'\n'+actual_finish+'\n'+network+'\n'+wire+'\n'+contrib+'\n'+tests)
 exe=p/'test'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++11','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(ROOT),'-I',str(LIB),'-I',str(JSON),str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
