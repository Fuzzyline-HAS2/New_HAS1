#!/usr/bin/env python3
"""Compile actual worker/facade and wire adapter with controlled scheduling.
No actual network, FreeRTOS scheduling or GPIO timing is claimed by this harness.
"""
from pathlib import Path
import os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
LIB=ROOT.parents[1]/'libraries/HAS2_Wifi'
JSON=Path(os.environ['ARDUINOJSON_INCLUDE'])
network=(ROOT/'network.ino').read_text().replace('#include <esp_task_wdt.h>','').replace('#include <esp_attr.h>','').replace('#include <esp_system.h>','')
contrib=(ROOT/'contrib.ino').read_text()
wifi=(ROOT/'wifi.ino').read_text()
actual_activate=wifi[wifi.index('void ActivateFunc(void)'):wifi.index('// game_state == \"ready\"')].replace('void ActivateFunc(void)', 'void ProductionActivateFunc(void)')
rfid=(ROOT/'rfid.ino').read_text()
actual_finish=rfid[rfid.index('void BatteryFinish()'): ]
actual_datachanged=wifi[wifi.index('JsonDocument cur;'):wifi.index('// 아무 동작도 하지 않는 대기 콜백.')].replace('void DataChanged()', 'void ProductionDataChanged()')
wire=(ROOT/'wire_http.ino').read_text().replace('#include <HTTPClient.h>','').replace('#include <has2_http_body.h>','')
prefix=r'''
#define HAS2_BT_HOST
#define GENERATOR_COMPLETION_STORE_HOST
#include "generator_completion_store.h"
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
static bool stored=false,saveOk=true,clearOk=true;
static GeneratorCompletionStore::Intent storedIntent={};
namespace GeneratorCompletionStore {
 bool save(const Intent &value){if(!saveOk)return false;storedIntent=value;stored=true;return true;}
 LoadResult load(Intent &value){if(!stored)return LoadResult::Empty;value=storedIntent;return LoadResult::Ready;}
 bool clear(){if(!clearOk)return false;stored=false;return true;}
}
static GeneratorWireSnapshot fakeContext;
bool WireCompletionContext(GeneratorWireSnapshot &value){value=fakeContext;return true;}
void WireRequestContextRefresh(){}
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
#define ESP_RST_SW 3
int esp_reset_reason(){return 1;}
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
struct Ota{void check(){assert(!worker);++otaChecks;otaTrace.emplace_back("ota");}}ota;
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
static bool bleResumeFails=false;
namespace Has1BleBeacon {
 enum class Step {Unavailable,Idle};
 struct Diagnostics {Step step=Step::Idle;unsigned lastError=0;};
 void setDeviceName(const char*){}
 void suspend(){otaTrace.emplace_back("ble-off");}
 void resume(){otaTrace.emplace_back("ble-on");}
 Diagnostics diagnostics(){Diagnostics d;if(bleResumeFails){d.step=Step::Unavailable;d.lastError=1;}return d;}
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
void WireResetTracking();
void TaggerRfidLoop(){}
void TaggerEnter(){ptrCurrentMode=TaggerRfidLoop;}
void BatteryFinish();void StartFinish();int StarterGaugeCnt();
bool batteryFinishDone=false,batteryFinishAudioPlayed=false,starterRfidNeedsValidation=false;
void WireResetTracking(){batteryFinishDone=batteryFinishAudioPlayed=false;serverWireBypass=false;GeneratorNetworkInvalidateGame();}
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
 otaTrace.clear();blackboxUploadIdle=true;bleResumeFails=false;
 pendingContributionCount=0;starterContribUser="";
 networkCompletionGeneration=0;networkCompletionToken=1;networkCompletion[0]=0;networkCompletionSent=networkCompletionVerify=networkCompletionConfirmed=networkCompletionSuperseded=networkCompletionDispatched=false;
 networkCompletionAttempts=0;networkCompletionResendId=0;networkCompletionDurable=networkCompletionStoreFault=networkCompletionRestored=networkCompletionAwaitResync=networkCompletionNeedsFull=false;stored=false;saveOk=clearOk=true;networkCompletionWireCount=-1;networkCompletionWireGeneration=0;
 duringWireSnapshot=nullptr;
 networkCompletionSince=networkCompletionWarningAt=networkCompletionRetryAt=0;
 GeneratorNetworkForgetTag();wireHttpPending=wireHttpDone=false;wireHttpRequestId=0;
 wireState.reset();for(int i=0;i<=10;++i){ wireState.sampleMask(i*100,0); }
 my["device_name"]="TG";my["game_state"]="activate";my["device_state"]="activate";
 strcpy(networkIdentity,"TG");strcpy(networkGame,"activate");
 now=1100;reply("{}");
}
static void completionSetup(){
 wireState.reset();for(int i=0;i<=10;++i)wireState.sampleMask(now+i*100,15);now+=1100;
 fakeContext=GeneratorWireSnapshot{};strcpy(fakeContext.device,"TG");strcpy(fakeContext.epoch,"11111111-1111-1111-1111-111111111111");
 strcpy(fakeContext.gameState,"activate");strcpy(fakeContext.deviceState,"activate");fakeContext.revision=7;fakeContext.count=4;fakeContext.maximum=4;fakeContext.completionProtocol=1;
 my["battery_pack"]=4;my["max_battery_pack"]=4;
}
static int completionCount(){int count=0;for(const auto &path:paths)if(path.find("CompleteGenerator")!=std::string::npos)++count;return count;}
static void casReply(const char *state,unsigned revision,int status=200,bool mutation=true,const char *epoch="11111111-1111-1111-1111-111111111111") {
 JsonDocument json;json["device_name"]="TG";json["epoch"]=epoch;json["revision"]=revision;json["game_state"]="activate";json["device_state"]=state;
 json["battery_pack"]=4;json["max_battery_pack"]=4;json["completion_protocol"]=1;json["success"]=status==200;if(status==409)json["error"]="CONFLICT";
 if(mutation)json["applied"]=status==200;
 std::string body;serializeJson(json,body);reply(body.c_str(),status);
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
 bool pollServed=false;for(size_t i=0;i<paths.size()&&i<4;++i)if(paths[i].find("request=Loop")!=std::string::npos)pollServed=true;assert(pollServed);
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
 blackboxUploadIdle=true;mainTurn();assert(!networkMaintenance&&otaChecks==1);
 assert((otaTrace==std::vector<std::string>{"pause","ble-off","ota","ble-on","unpause"}));
 {bool logged=false;for(const auto &line:diagnostics){if(line.find("OTA memory")!=std::string::npos){logged=true;}}assert(logged);}
 assert(restarts==0);
 pass("GitHub OTA waits for the MQTT session to end, then stops BLE only around the check");

 reset();bleResumeFails=true;GeneratorNetworkRequestMaintenance();mainTurn();
 assert(otaChecks==1&&!networkMaintenance&&restarts==1);
 assert((otaTrace==std::vector<std::string>{"pause","ble-off","ota","ble-on","unpause"}));
 pass("BLE that does not come back after a failed OTA restarts the device rather than play without a beacon");

 reset();blackboxUploadIdle=false;GeneratorNetworkRequestMaintenance();
 now+=14999;mainTurn();assert(otaChecks==0);
 now+=1;mainTurn();assert(otaChecks==1&&!networkMaintenance);
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

 // Completion protocol is opt-in and never falls back to an unfenced Send.
 reset();completionSetup();fakeContext.completionProtocol=0;
 assert(!GeneratorNetworkComplete("battery_max")&&used()==0&&!stored);
 pass("missing server capability cannot send legacy completion");

 reset();completionSetup();saveOk=false;
 assert(!GeneratorNetworkComplete("battery_max")&&requestCalls==0&&used()==0&&networkCompletionStoreFault);
 pass("journal failure prevents all completion transmission");

 reset();completionSetup();assert(!GeneratorNetworkComplete("battery_max"));assert(stored&&used()==1);
 const std::string originalEpoch=storedIntent.epoch;const auto originalRevision=storedIntent.revision;
 casReply("battery_max",8);workerTurn();mainTurn();
 assert(networkCompletionConfirmed&&!stored&&GeneratorNetworkComplete("battery_max"));
 assert(paths[0].find("request=CompleteGenerator")!=std::string::npos&&paths[0].find("revision=7")!=std::string::npos);
 pass("durable conditional request confirms snapshot and clears journal");

 reset();completionSetup();assert(!GeneratorNetworkComplete("battery_max"));
 failAt(Has2BoundedTransport::Phase::Header);workerTurn();mainTurn();
 assert(networkCompletionDispatched&&stored&&!networkCompletionConfirmed);
 now=networkCompletionRetryAt;mainTurn();casReply("battery_max",8,200,false);workerTurn();mainTurn();
 assert(networkCompletionConfirmed&&!stored&&completionCount()==1);
 pass("lost applied response reconciles completed state without a second mutation");

 reset();completionSetup();assert(!GeneratorNetworkComplete("battery_max"));
 for(int i=0;i<5;++i) {
   failAt(i%2?Has2BoundedTransport::Phase::Header:Has2BoundedTransport::Phase::Connect);workerTurn();mainTurn();
   assert(stored&&!networkCompletionConfirmed&&queuedCompletions()==0);
   const auto deadline=networkCompletionRetryAt;
   now=deadline-1;mainTurn();assert(used()==0);
   now=deadline;mainTurn();casReply("activate",7,200,false);workerTurn();mainTurn();
   assert(queuedCompletions()==1&&storedIntent.revision==7&&!strcmp(storedIntent.epoch,originalEpoch.c_str()));
 }
 casReply("battery_max",8,409);workerTurn();mainTurn();
 assert(completionCount()==6&&networkCompletionConfirmed&&sendCount()==0);
 for(const auto &path:paths) if(path.find("CompleteGenerator")!=std::string::npos) assert(path.find("revision=7")!=std::string::npos);
 assert(originalRevision==7);
 pass("connect and uncertain failures retry beyond three with backoff and immutable original token");

 reset();completionSetup();assert(!GeneratorNetworkComplete("battery_max"));
 casReply("activate",9,409);workerTurn();mainTurn();
 assert(networkCompletionSuperseded&&networkCompletionAwaitResync&&!stored);
 assert(!GeneratorNetworkComplete("battery_max")&&completionCount()==1);
 pass("changed revision abandons old gameplay intent until authoritative cycle resync");

 reset();completionSetup();assert(!GeneratorNetworkComplete("battery_max"));
 casReply("activate",7,409,true,"22222222-2222-2222-2222-222222222222");workerTurn();mainTurn();
 assert(networkCompletionSuperseded&&networkResetCyclePending&&!stored&&completionCount()==1);
 pass("server epoch change never retokens old work");

 reset();completionSetup();assert(!GeneratorNetworkComplete("battery_max"));
 failAt(Has2BoundedTransport::Phase::Header);workerTurn();mainTurn();
 for(int i=1;i<=10;++i)wireState.sampleMask(now+i*100,7);
 now=networkCompletionRetryAt;mainTurn();casReply("activate",7,200,false);workerTurn();mainTurn();
 assert(networkCompletionSuperseded&&queuedCompletions()==0&&!stored);
 pass("fresh physical wire removal prevents uncertain completion retry");

 reset();completionSetup();assert(!GeneratorNetworkComplete("battery_max"));
 failAt(Has2BoundedTransport::Phase::Header);workerTurn();mainTurn();
 // Emulate new volatile state with the same committed journal.
 networkCompletionSent=networkCompletionDispatched=networkCompletionConfirmed=networkCompletionSuperseded=false;
 networkCompletionDurable=false;networkCompletionGeneration=0;networkCompletionResendId=0;
 NetworkCompletionLoad();assert(networkCompletionRestored&&GeneratorNetworkCompletionPending());
 assert(!GeneratorNetworkComplete("battery_max")&&queuedCompletions()==0);
 casReply("activate",7,200,false);workerTurn();mainTurn();
 assert(!networkCompletionRestored&&queuedCompletions()==1&&storedIntent.revision==7);
 casReply("battery_max",8,409);workerTurn();mainTurn();assert(networkCompletionConfirmed&&!stored);
 pass("restored journal requires token-bearing read before retrying original operation");

 reset();completionSetup();assert(!GeneratorNetworkComplete("battery_max"));
 clearOk=false;casReply("battery_max",8);workerTurn();mainTurn();
 assert(networkCompletionStoreFault&&stored&&!GeneratorNetworkComplete("repaired"));
 pass("journal clear failure blocks subsequent intents");

 reset();completionSetup();assert(!GeneratorNetworkComplete("battery_max"));
 reply("{\"device_name\":\"TG\",\"game_state\":\"activate\",\"device_state\":\"battery_max\"}");workerTurn();mainTurn();
 assert(!networkCompletionConfirmed&&stored);
 pass("legacy partial JSON cannot confirm a conditional completion");

 reset();completionSetup();assert(!GeneratorNetworkComplete("battery_max"));
 duringWireSnapshot=[](){++networkGeneration;};workerTurn();mainTurn();assert(requestCalls==0);
 pass("game change during dispatch preflight still rejects completion");

 reset();completionSetup();assert(GeneratorNetworkContribution("G1P1",1));
 assert(!GeneratorNetworkComplete("battery_max"));casReply("battery_max",8);workerTurn();
 assert(paths[0].find("CompleteGenerator")!=std::string::npos);
 pass("completion dispatch precedes older optional contribution transmission");

 reset();completionSetup();my["left_generator"]="3";cur.set(my);exerciseActualDataChanged=true;ptrCurrentMode=WirePollMain;
 BatteryFinish();assert(!batteryFinishDone&&stored);
 casReply("battery_max",8);workerTurn();mainTurn();BatteryFinish();
 assert(batteryFinishDone&&ptrCurrentMode==StarterActivate&&!serverWireBypass);
 std::string full;serializeJson(my,full);reply(full.c_str());workerTurn();mainTurn();
 fakeContext.revision=8;strcpy(fakeContext.deviceState,"battery_max");
 StartFinish();assert(stored&&!networkCompletionConfirmed);
 casReply("repaired",9);workerTurn();mainTurn();
 assert(networkCompletionConfirmed&&networkCompletionNeedsFull&&!GeneratorNetworkComplete("repaired"));
 JsonDocument complete;complete.set(my);complete["device_state"]="repaired";complete["left_generator"]="2";
 full.clear();serializeJson(complete,full);reply(full.c_str());workerTurn();mainTurn();
 assert(!networkCompletionNeedsFull&&ptrCurrentMode==WaitFunc&&!serverWireBypass&&!strcmp(my["left_generator"]|"","2"));
 pass("actual BatteryFinish and StartFinish consume conditional confirmations without staff bypass");

 reset();completionSetup();my["left_generator"]="3";cur.set(my);exerciseActualDataChanged=true;ptrCurrentMode=WirePollMain;
 assert(!GeneratorNetworkComplete("battery_max"));failAt(Has2BoundedTransport::Phase::Header);workerTurn();mainTurn();
 networkCompletionSent=networkCompletionConfirmed=networkCompletionSuperseded=false;networkCompletionGeneration=0;
 NetworkCompletionLoad();NetworkCompletionService();casReply("battery_max",8,200,false);workerTurn();mainTurn();
 assert(networkCompletionConfirmed&&GeneratorNetworkComplete("battery_max"));BatteryFinish();
 assert(ptrCurrentMode==StarterActivate&&batteryFinishDone&&!serverWireBypass);
 pass("already-applied restored battery intent binds boot generation and advances actual gameplay");

 reset();completionSetup();saveOk=false;assert(!GeneratorNetworkComplete("battery_max"));
 NetworkCompletionService();assert(used()==0&&networkCompletionStoreFault);
 my["game_state"]="ready";NetworkCompletionService();assert(used()==1);
 casReply("activate",7,200,false);workerTurn();mainTurn();assert(networkCompletionStoreFault);
 now=networkCompletionRetryAt;NetworkCompletionService();
 casReply("activate",7,200,false);JsonDocument safe;deserializeJson(safe,nextReply.payload);safe["game_state"]="ready";
 std::string safeBody;serializeJson(safe,safeBody);reply(safeBody.c_str());workerTurn();mainTurn();
 assert(!networkCompletionStoreFault&&networkCompletionAwaitResync&&!stored);
 pass("journal fault only clears after fresh matching inactive game boundary, never during active play");

 reset();completionSetup();my["device_state"]="battery_max";my["left_generator"]="3";cur.set(my);exerciseActualDataChanged=true;
 strcpy(fakeContext.deviceState,"battery_max");batteryFinishDone=true;encoderValue=1000;displayedGaugeNeoCnt=28;ptrCurrentMode=StarterActivate;
 StartFinish();assert(stored);casReply("battery_max",9,409);workerTurn();mainTurn();
 assert(networkCompletionSuperseded&&!batteryFinishDone&&networkCompletionAwaitResync);
 std::string resynced;serializeJson(my,resynced);reply(resynced.c_str());workerTurn();mainTurn();
 assert(ptrCurrentMode==BatteryFinish);ptrCurrentMode();
 assert(!networkCompletionAwaitResync&&batteryFinishDone&&encoderValue==1&&displayedGaugeNeoCnt<28&&ptrCurrentMode==StarterActivate);
 assert(queuedCompletions()==0&&completionCount()==1);
 pass("stale repair resync resets actual starter gauge before any new accomplishment can create a token");

 for(bool identityChange:{false,true}) {
   reset();completionSetup();my["left_generator"]="3";cur.set(my);exerciseActualDataChanged=true;
   assert(!GeneratorNetworkComplete("battery_max"));failAt(Has2BoundedTransport::Phase::Header);workerTurn();mainTurn();
   now=networkCompletionRetryAt;mainTurn();casReply("battery_max",8,200,false);
   duringRequest=[ ](){my["game_state"]="ready";GeneratorNetworkInvalidateGame();};
   if(identityChange)duringRequest=[](){my["device_name"]="OTHER";my["game_state"]="ready";GeneratorNetworkInvalidateGame();};
   workerTurn();mainTurn();assert(!networkCompletionConfirmed&&stored&&!strcmp(my["game_state"]|"","ready")&&activations==0);
   if(identityChange)assert(!strcmp(my["device_name"]|"","OTHER"));
 }
 pass("late verification after game or identity reset cannot resurrect old presentation");

 reset();completionSetup();my["left_generator"]="3";cur.set(my);exerciseActualDataChanged=true;
 assert(!GeneratorNetworkComplete("battery_max"));failAt(Has2BoundedTransport::Phase::Header);workerTurn();mainTurn();
 networkCompletionSent=networkCompletionConfirmed=networkCompletionSuperseded=false;networkCompletionGeneration=0;
 NetworkCompletionLoad();NetworkCompletionService();casReply("battery_max",8,200,false);
 duringRequest=[](){my["game_state"]="ready";GeneratorNetworkInvalidateGame();my["game_state"]="activate";GeneratorNetworkInvalidateGame();};
 workerTurn();mainTurn();assert(!networkCompletionConfirmed&&stored&&networkCompletionRestored&&!strcmp(my["device_state"]|"","activate"));
 now=networkCompletionRetryAt;mainTurn();casReply("activate",7,200,false);workerTurn();mainTurn();
 assert(queuedCompletions()==1&&!networkCompletionRestored&&storedIntent.revision==7);
 pass("restored verification crossing a local reset is discarded; a new generation read reconciles the original token");

 for(unsigned staleRevision:{6u,7u}) {
   reset();completionSetup();assert(!GeneratorNetworkComplete("battery_max"));
   casReply("battery_max",staleRevision);workerTurn();mainTurn();
   assert(stored&&!networkCompletionConfirmed&&!networkCompletionSuperseded&&!strcmp(my["device_state"]|"","activate"));
   now=networkCompletionRetryAt;mainTurn();casReply("activate",6,200,false);workerTurn();mainTurn();
   assert(stored&&queuedCompletions()==0&&!networkCompletionSuperseded&&storedIntent.revision==7);
 }
 pass("older revision and impossible equal-revision completion cannot clear or confirm the original journal");

 reset();completionSetup();assert(!GeneratorNetworkComplete("battery_max"));
 memset(storedIntent.deviceKey,'G',sizeof(storedIntent.deviceKey)-1);storedIntent.deviceKey[sizeof(storedIntent.deviceKey)-1]=0;
 networkCompletionSent=false;NetworkCompletionLoad();
 assert(strlen(networkCompletionDevice)==64&&!strcmp(networkCompletionDevice,storedIntent.deviceKey));
 assert(sizeof(networkCompletionStartState)==sizeof(GeneratorWireSnapshot::deviceState));
 pass("journal restore preserves all 64 valid key characters and snapshot state capacity matches its source");

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

 reset();completionSetup();assert(!GeneratorNetworkComplete("battery_max"));
 failAt(Has2BoundedTransport::Phase::Header);workerTurn();mainTurn();
 online=false;mainTurn();now+=180000;workerTurn();mainTurn();
 assert(restarts==1&&networkRecoveryFence&&stored&&completionCount()==1);
 pass("durable uncertain completion permits offline reboot without losing retry identity");

 reset();online=false;assert(!NetworkOfflineRecoveryService());now+=180000;
 reconnectOnRecheck=true;wifiStatusCalls=0;
 assert(!NetworkOfflineRecoveryService()&&online&&restarts==0&&!networkRecoveryFence&&networkRecoveryPolicy.hadConnection);
 pass("live association recheck after fencing prevents a stale-offline restart");

 reset();assert(GeneratorNetworkRequestPoll());networkRecoveryFence=true;workerTurn();
 assert(requestCalls==0);assert(!GeneratorNetworkContribution("G1P1",1));
 pass("restart fence prevents worker selection and new enqueue");

 reset();completionSetup();assert(!GeneratorNetworkComplete("battery_max"));
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
