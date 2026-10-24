// One bounded transport owner. Queue metadata is fixed; at most one response
// body exists between the worker and main. Only main owns game/JSON state.
#include "generator_network.h"
#include <has2_bounded_transport.h>
#include <esp_task_wdt.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_timer.h>
#include "generator_offline_recovery.h"

static constexpr unsigned kNetworkSlots=8;
static constexpr uint32_t kNetworkReadLifetime=10000;
struct NetworkSlot { NetworkRequest request; bool occupied=false; bool active=false; };
static NetworkSlot networkSlots[kNetworkSlots];
static portMUX_TYPE networkMux=portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t networkTask=nullptr;
static uint32_t networkGeneration=1, networkNextId=1;
static bool networkMailboxReady=false;
static int networkMailboxSlot=-1;
static NetworkDiscard networkMailboxDiscard=NetworkDiscard::None;
static bool networkMailboxCancelled=false;
static uint32_t networkWorkerStackFree=0;
static Has2BoundedTransport::Result networkMailbox;
static bool networkPollPending=false,networkPollEvent=false,networkPollSuccess=false,networkPollAwaitingRefresh=false;
static unsigned networkUrgentRun=0;
static bool networkScanPending=false,networkDriverFault=false;
static uint32_t networkMaintenanceStarted=0;
static Has2BoundedTransport::Phase networkWorkerPhase=Has2BoundedTransport::Phase::Connect;
static uint32_t networkWorkerActiveId=0,networkWorkerStarted=0,networkWorkerProgressAt=0;
static bool networkRefreshPending=true,networkRefreshInFlight=false,networkBootReady=false,networkResetCyclePending=false;
static bool networkVersionSent=false,networkWatchdogPending=false,networkWatchdogRequested=false;
static bool networkMaintenance=false,networkMaintenanceLegacy=false;
static bool networkOtaRestartPending=false;
static uint32_t networkOtaRestartSince=0;
static bool networkWasConnected=false;
RTC_NOINIT_ATTR static GeneratorOfflineRecovery::Retained networkRecoveryHistory;
// GitHub OTA attempts started while the server stayed at github. Counted before
// the check, so a panic or watchdog reset inside it counts too. Survives those
// and software resets; power-on and any other device_state clear it.
struct NetworkOtaRetained { uint32_t magic,attempts,check; };
RTC_NOINIT_ATTR static NetworkOtaRetained networkOtaRetained;
static constexpr uint32_t kNetworkOtaMagic=0x4f544131u,kNetworkOtaMaxAttempts=3;
static uint32_t NetworkOtaAttempts() {
  const NetworkOtaRetained &r=networkOtaRetained;
  return r.magic==kNetworkOtaMagic&&r.check==(r.attempts^0x5a17c3e9u)&&r.attempts<=kNetworkOtaMaxAttempts?r.attempts:0;
}
static void NetworkOtaAttemptsSet(uint32_t attempts) {
  networkOtaRetained={kNetworkOtaMagic,attempts,attempts^0x5a17c3e9u};
}
static GeneratorOfflineRecovery::Policy networkRecoveryPolicy;
static bool networkRecoveryFence=false;
static uint32_t networkRecoverySalt=0,networkRecoveryLastLog=0;
static bool networkRecoveryBlockedLogged=false;
static uint32_t networkRecoveryBlockedReason=0;
static uint32_t networkRefreshAt=0;
static char networkIdentity[32]={},networkGame[32]={};

static char networkTagKey[16]={};
static uint32_t networkTagToken=1,networkTagRequest=0,networkTagRetryAt=0;
static bool networkTagReady=false;
static uint8_t networkTagFailures=0;
static uint32_t networkCompletionToken=1;
static void NetworkCompletionObserveSnapshot();
static bool NetworkOwnBatteryEcho(const JsonDocument &snapshot);
static void NetworkCompletionResendSubmit();
static String NetworkEncode(const char *text) {
  static const char hex[]="0123456789ABCDEF";
  String out;
  if (!text) { return out; }
  for (const unsigned char *p=(const unsigned char*)text;*p;++p) {
    const unsigned char c=*p;
    if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') { out+=char(c); }
    else { out+='%';out+=hex[c>>4];out+=hex[c&15]; }
  }
  return out;
}
uint32_t GeneratorNetworkGeneration() {
  portENTER_CRITICAL(&networkMux);const uint32_t result=networkGeneration;portEXIT_CRITICAL(&networkMux);return result;
}
static uint32_t NetworkSubmit(NetworkKind kind,const String &path,bool mutation,uint32_t generation,uint32_t token,
    void (*callback)(const NetworkRequest &,const Has2BoundedTransport::Result &,bool),int expectedWireCount=-1,uint32_t wireGeneration=0,bool wireBypass=false) {
  if (networkMaintenance&&kind!=NetworkKind::Report){ return 0; }
  if (path.length()>=sizeof(NetworkRequest::path)) { return 0; }
  NetworkRequest request={};
  memcpy(request.path,path.c_str(),path.length()+1);
  request.kind=kind;request.mutation=mutation;request.generation=generation;request.token=token;
  request.enqueued=millis();request.callback=callback;request.expectedWireCount=expectedWireCount;request.wireGeneration=wireGeneration;request.wireBypass=wireBypass;
  portENTER_CRITICAL(&networkMux);
  unsigned used=0;for (const auto &slot:networkSlots) if(slot.occupied){ ++used; }
  // Leave capacity for heartbeat/state repair when optional reports fill the queue.
  const bool optional=kind==NetworkKind::Contribution||kind==NetworkKind::Report;
  if (networkTask&&!networkRecoveryFence&&!(optional&&used>=kNetworkSlots-2)&&!(kind!=NetworkKind::Poll&&used>=kNetworkSlots-1)) {
    for(auto &slot:networkSlots) if(!slot.occupied) {
      request.id=networkNextId++;if(!networkNextId){ ++networkNextId; }
      slot.request=request;slot.occupied=true;slot.active=false;break;
    }
  }
  portEXIT_CRITICAL(&networkMux);
  if(request.id) has1blackbox::blackbox.setUploadAllowed(false);
  return request.id;
}
void GeneratorNetworkInvalidateGame() {
  portENTER_CRITICAL(&networkMux);++networkGeneration;if(!networkGeneration){ ++networkGeneration; }portEXIT_CRITICAL(&networkMux);
  GeneratorNetworkForgetTag();
}
static NetworkDiscard NetworkDiscardReason(const NetworkRequest &request) {
  if(request.cancelled){ return NetworkDiscard::Cancelled; }
  if(request.kind==NetworkKind::Tag) {
    portENTER_CRITICAL(&networkMux);const uint32_t token=networkTagToken;portEXIT_CRITICAL(&networkMux);
    if(request.token!=token){ return NetworkDiscard::TagChanged; }
  }
  if(request.generation&&request.generation!=GeneratorNetworkGeneration()){ return NetworkDiscard::GameChanged; }
  if(request.kind==NetworkKind::Completion||request.kind==NetworkKind::Verify) {
    portENTER_CRITICAL(&networkMux);const uint32_t token=networkCompletionToken;portEXIT_CRITICAL(&networkMux);
    if(request.token!=token){ return NetworkDiscard::Cancelled; }
  }
  if(request.kind==NetworkKind::Completion&&!request.dispatched&&!request.wireBypass) {
    const GeneratorWireState wire=WireSnapshot();
    if(request.wireGeneration!=wire.generation()){ return NetworkDiscard::WireChanged; }
    if(!wire.qualified(millis())||wire.stable()!=request.expectedWireCount){ return NetworkDiscard::WireUnstable; }
  }
  if(request.kind==NetworkKind::Wire) {
    const GeneratorWireState wire=WireSnapshot();
    if(request.token!=wire.generation()){ return NetworkDiscard::WireChanged; }
    if(request.mutation&&(!wire.qualified(millis())||wire.stable()!=request.expectedWireCount)){ return NetworkDiscard::WireUnstable; }
  }
  return NetworkDiscard::None;
}
static const char *NetworkDiscardName(NetworkDiscard reason) {
  switch(reason) {
    case NetworkDiscard::None:return "none";
    case NetworkDiscard::Cancelled:return "cancelled";
    case NetworkDiscard::TagChanged:return "tag_changed";
    case NetworkDiscard::GameChanged:return "game_changed";
    case NetworkDiscard::WireChanged:return "wire_changed";
    case NetworkDiscard::WireUnstable:return "wire_unstable";
    case NetworkDiscard::QueueExpired:return "queue_expired";
    case NetworkDiscard::ResultExpired:return "result_expired";
  }
  return "unknown";
}
static bool NetworkCurrent(const NetworkRequest &request) {return NetworkDiscardReason(request)==NetworkDiscard::None;}
static bool NetworkReplaceQueued(uint32_t id,const String &path,uint32_t generation,uint32_t token,int expectedWireCount) {
  if(path.length()>=sizeof(NetworkRequest::path)){ return false; }
  bool replaced=false;
  portENTER_CRITICAL(&networkMux);
  for(auto &slot:networkSlots)if(slot.occupied&&slot.request.id==id&&!slot.active) {
    memcpy(slot.request.path,path.c_str(),path.length()+1);slot.request.generation=generation;
    slot.request.token=token;slot.request.expectedWireCount=expectedWireCount;slot.request.mutation=expectedWireCount>=0;replaced=true;break;
  }
  portEXIT_CRITICAL(&networkMux);return replaced;
}
static void NetworkWorkerProgress() {
  esp_task_wdt_reset();
  portENTER_CRITICAL(&networkMux);networkWorkerProgressAt=millis();portEXIT_CRITICAL(&networkMux);
}
static void NetworkWorkerPhaseChanged(Has2BoundedTransport::Phase phase) {
  portENTER_CRITICAL(&networkMux);networkWorkerPhase=phase;portEXIT_CRITICAL(&networkMux);
}
static void NetworkWorker(void *) {
  const esp_err_t watched=esp_task_wdt_add(nullptr);
  if(watched!=ESP_OK) {
    DiagnosticPrintf("[NET] worker watchdog registration failed=%d; network disabled\r\n",int(watched));
    portENTER_CRITICAL(&networkMux);networkTask=nullptr;portEXIT_CRITICAL(&networkMux);
    vTaskDelete(nullptr);return;
  }
  for (;;) {
    esp_task_wdt_reset();
    const bool online=has2wifi.ServiceConnection();
    const bool scanPending=has2wifi.ConnectionScanPending();
    if(has2wifi.ConnectionDriverFault()) {
      portENTER_CRITICAL(&networkMux);networkDriverFault=true;portEXIT_CRITICAL(&networkMux);
      // An ESP-IDF scan never completed even after cancellation grace. Do not
      // free/replace its buffer or pretend this is an ordinary offline retry.
      // This worker alone stops feeding its registered watchdog.
      for(;;){ vTaskDelay(pdMS_TO_TICKS(100)); }
    }
    const uint32_t stackFree=uxTaskGetStackHighWaterMark(nullptr);
    portENTER_CRITICAL(&networkMux);
    networkScanPending=scanPending;
    networkWorkerStackFree=stackFree;
    const bool mailboxBusy=networkMailboxReady;
    int selected=-1;uint32_t oldestAge=0;int poll=-1;
    const uint32_t now=millis();
    if (!mailboxBusy&&!networkRecoveryFence) {
      for(unsigned i=0;i<kNetworkSlots;++i) if(networkSlots[i].occupied&&!networkSlots[i].active) {
        const NetworkRequest &r=networkSlots[i].request;
        const uint32_t age=now-r.enqueued;
        if(!online&&!r.cancelled&&(!r.generation||r.generation==networkGeneration)&&(r.mutation||age<=kNetworkReadLifetime)){ continue; }
        if(r.kind==NetworkKind::Poll){ poll=int(i); }
        if(selected<0||age>oldestAge){selected=int(i);oldestAge=age;}
      }
      // A heartbeat waits behind at most three other completed requests.
      if(poll>=0&&(networkUrgentRun>=3||now-networkSlots[poll].request.enqueued>=4000)){ selected=poll; }
      if(selected>=0){networkSlots[selected].active=true;networkSlots[selected].request.started=now;networkWorkerActiveId=networkSlots[selected].request.id;networkWorkerStarted=now;networkWorkerProgressAt=now;}
    }
    NetworkRequest request={};if(selected>=0){ request=networkSlots[selected].request; }
    portEXIT_CRITICAL(&networkMux);
    if(selected<0){vTaskDelay(pdMS_TO_TICKS(25));continue;}
    networkUrgentRun=request.kind==NetworkKind::Poll?0:networkUrgentRun+1;
    Has2BoundedTransport::Result result;
    NetworkDiscard discard=NetworkDiscardReason(request);
    if(discard==NetworkDiscard::None&&!request.mutation&&uint32_t(millis()-request.enqueued)>kNetworkReadLifetime){ discard=NetworkDiscard::QueueExpired; }
    if(discard==NetworkDiscard::None) {
      // This lock is the dispatch boundary: a cancellation published before it
      // must win. Once dispatched is published, the outcome is conservatively
      // uncertain even if a later local cancellation arrives before socket I/O.
      portENTER_CRITICAL(&networkMux);
      const NetworkRequest &current=networkSlots[selected].request;
      if(networkRecoveryFence||current.cancelled){ discard=NetworkDiscard::Cancelled; }
      else if(request.generation&&request.generation!=networkGeneration){ discard=NetworkDiscard::GameChanged; }
      else if(request.kind==NetworkKind::Tag&&request.token!=networkTagToken){ discard=NetworkDiscard::TagChanged; }
      else if((request.kind==NetworkKind::Completion||request.kind==NetworkKind::Verify)&&request.token!=networkCompletionToken){ discard=NetworkDiscard::Cancelled; }
      else {request.dispatched=true;networkSlots[selected].request.dispatched=true;}
      portEXIT_CRITICAL(&networkMux);
    }
    const bool cancelled=discard!=NetworkDiscard::None;
    if(!cancelled) {
      Has2BoundedTransport::Options options;
      options.deadlineMs=2000;options.progressHook=NetworkWorkerProgress;options.phaseHook=NetworkWorkerPhaseChanged;
      if(request.kind==NetworkKind::Wire){options.bodyDeadlineMs=1000;options.maxBodyBytes=1024;options.requireContentLength=true;}
      result=Has2BoundedTransport::get("172.30.1.43",80,request.path,options);
    }
    // Single owner until ready is published; main clears it only after processing.
    networkMailbox=std::move(result);
    portENTER_CRITICAL(&networkMux);
    networkMailboxSlot=selected;networkMailboxCancelled=cancelled;networkMailboxDiscard=discard;networkMailboxReady=true;
    portEXIT_CRITICAL(&networkMux);
  }
}
static bool NetworkOk(const Has2BoundedTransport::Result &result,bool cancelled) {
  return !cancelled&&result.ok()&&result.status==200;
}
static void NetworkIdentityChanged() {
  const char *name=my["device_name"]|"";const char *game=my["game_state"]|"";
  if(strcmp(name,networkIdentity)||strcmp(game,networkGame)) {
    snprintf(networkIdentity,sizeof(networkIdentity),"%s",name);
    snprintf(networkGame,sizeof(networkGame),"%s",game);
    GeneratorNetworkInvalidateGame();
  }
}
static bool NetworkApplySnapshot(const Has2BoundedTransport::Result &result,bool cancelled,bool localCompletion=false) {
  if(!NetworkOk(result,cancelled)){ return false; }
  JsonDocument candidate;
  if(deserializeJson(candidate,result.payload)||!candidate["device_name"].is<const char*>()){ return false; }
  const char *identity=candidate["device_name"].as<const char*>();
  if(!identity[0]||strlen(identity)>=sizeof(networkIdentity)){ return false; }
  const bool unchangedState=!strcmp(candidate["game_state"] | "",my["game_state"] | "")&&
      !strcmp(candidate["device_state"] | "",my["device_state"] | "");
  localCompletion=localCompletion||NetworkOwnBatteryEcho(candidate);
  if(!has2wifi.ApplyNetworkSnapshot("ReceiveMine",result.payload)){ return false; }
  NetworkIdentityChanged();networkBootReady=true;networkRecoveryPolicy.serverReply(millis());
  if(!unchangedState||networkResetCyclePending) {
    // Close before a same-game mode override can reset the encoder. A new game
    // generation deliberately discards the prior session in ContribEnd.
    ContribEnd(starterContribLastCnt);starterLastTagState=false;
  }
  receiveMineOn=localCompletion;
  DataChanged();
  if(networkResetCyclePending) {
    networkResetCyclePending=false;
    if(unchangedState) {
      const char *game=my["game_state"]|"";
      const char *state=my["device_state"]|"";
      if(!strcmp(game,"activate")&&(!strcmp(state,"activate")||!strcmp(state,"battery_max")||!strcmp(state,"starter_finish"))){ ActivateFunc(); }
      else if(!strcmp(game,"setting")){ SettingFunc(); }
      else if(!strcmp(game,"ready")){ ReadyFunc(); }
    }
  }
  NetworkCompletionObserveSnapshot();
  if(!strcmp(my["device_state"] | "", "update")){ GeneratorNetworkRequestMaintenance(true); }
  if(strcmp(my["device_state"] | "", "github")&&NetworkOtaAttempts()){ NetworkOtaAttemptsSet(0); }
  return true;
}
static void NetworkFinishPoll(bool success) {
  networkPollPending=false;networkPollAwaitingRefresh=false;networkPollEvent=true;networkPollSuccess=success;
}
static void NetworkWatchdogResult(const NetworkRequest &,const Has2BoundedTransport::Result &,bool) {
  networkWatchdogPending=false;
  // Even HTTP 200 may be an application error/ignored mutation. A subsequent
  // fresh Loop must confirm watchdog=0 before honoring the cached staff command.
}
static void NetworkRefreshResult(const NetworkRequest &,const Has2BoundedTransport::Result &result,bool cancelled) {
  networkRefreshInFlight=false;
  const bool ok=NetworkApplySnapshot(result,cancelled);
  BlackBoxValidSnapshot(ok);
  networkRefreshPending=!ok;networkRefreshAt=millis()+2000;
  if(networkPollAwaitingRefresh){ NetworkFinishPoll(ok); }
}
static bool NetworkQueueRefresh() {
  if(networkRefreshInFlight){ return true; }
  String path="/has2.php?request=ReceiveMine&table=device&mac="+NetworkEncode(has2wifi.GetMacAddress().c_str());
  if(!NetworkSubmit(NetworkKind::Refresh,path,false,0,0,NetworkRefreshResult)){ return false; }
  networkRefreshInFlight=true;return true;
}
static void NetworkPollResult(const NetworkRequest &,const Has2BoundedTransport::Result &result,bool cancelled) {
  if(!NetworkOk(result,cancelled)||!has2wifi.ApplyNetworkSnapshot("Loop",result.payload)){BlackBoxPollResult(false);NetworkFinishPoll(false);return;}
  BlackBoxPollResult(true);networkRecoveryPolicy.serverReply(millis());
  if((int)shift_machine["watchdog"]>=1){ networkWatchdogRequested=true; }
  else if(networkWatchdogRequested&&!networkWatchdogPending) {
    DiagnosticPrintf("[NET] explicit server reboot clear confirmed by poll\r\n");BlackBoxBeforeRestart();ESP.restart();
  }
  if(networkWatchdogRequested&&!networkWatchdogPending&&networkBootReady) {
    String path="/has2.php?request=Send&table=device&key="+NetworkEncode(my["device_name"]|"")+"&column=watchdog&value=0";
    networkWatchdogPending=NetworkSubmit(NetworkKind::Watchdog,path,true,0,0,NetworkWatchdogResult)!=0;
    DiagnosticPoint(GeneratorDiagnostic::ServerReset,1,1);
  }
  if((int)shift_machine["shift_machine"]>=1){ networkRefreshPending=true; }
  if(networkRefreshPending) { networkPollAwaitingRefresh=true;if(!NetworkQueueRefresh()){ NetworkFinishPoll(false); } }
  else NetworkFinishPoll(true);
}
bool GeneratorNetworkRequestPoll() {
  if(networkPollPending){ return true; }
  String path="/has2.php?request=Loop&table=device&mac="+NetworkEncode(has2wifi.GetMacAddress().c_str());
  if(!NetworkSubmit(NetworkKind::Poll,path,false,0,0,NetworkPollResult)){ return false; }
  networkPollPending=true;return true;
}
bool GeneratorNetworkPollBusy(){return networkPollPending;}
bool GeneratorNetworkTakePollResult(bool &success) {
  if (!networkPollEvent) {
    return false;
  }
  networkPollEvent=false;
  success=networkPollSuccess;
  return true;
}
void GeneratorNetworkRefresh(){networkRefreshPending=true;networkRefreshAt=millis();}
void GeneratorNetworkRefreshCycle(){networkResetCyclePending=true;GeneratorNetworkRefresh();}
static void NetworkReportResult(const NetworkRequest &request,const Has2BoundedTransport::Result &result,bool cancelled) {
  if(!NetworkOk(result,cancelled)){ DiagnosticPrintf("[NET] report id=%lu not confirmed; local diagnostics retained\r\n",(unsigned long)request.id); }
}
bool GeneratorNetworkSend(const char *device,const char *column,const char *value) {
  if(!device||!device[0]||!column||!value){ return false; }
  String path="/has2.php?request=Send&table=device&key="+NetworkEncode(device)+"&column="+NetworkEncode(column)+"&value="+NetworkEncode(value);
  return NetworkSubmit(NetworkKind::Report,path,true,0,0,NetworkReportResult)!=0;
}
bool GeneratorNetworkContribution(const char *user,int delta) {
  if(!networkBootReady||!user||delta<1){ return false; }

  String value=String(user)+":"+String(delta);
  String path="/has2.php?request=Situation&table=generator_gauge&key="+NetworkEncode(my["device_name"]|"")+"&value="+NetworkEncode(value.c_str());
  return NetworkSubmit(NetworkKind::Contribution,path,true,GeneratorNetworkGeneration(),0,NetworkReportResult)!=0;
}
void GeneratorNetworkForgetTag(){
  portENTER_CRITICAL(&networkMux);
  ++networkTagToken;if(!networkTagToken){ ++networkTagToken; }
  // Old inactive card lookups have no side effects and no consumer after this
  // token change. Remove them now rather than occupying all control slots.
  for(auto &slot:networkSlots)if(slot.occupied&&!slot.active&&slot.request.kind==NetworkKind::Tag){ slot.occupied=false; }
  portEXIT_CRITICAL(&networkMux);
  networkTagRequest=0;networkTagKey[0]=0;networkTagReady=false;networkTagFailures=0;networkTagRetryAt=0;tag.clear();
}
static void NetworkTagResult(const NetworkRequest &request,const Has2BoundedTransport::Result &result,bool cancelled) {
  if(request.token!=networkTagToken){ return; }
  networkTagRequest=0;
  networkTagReady=NetworkOk(result,cancelled)&&has2wifi.ApplyNetworkSnapshot("Receive",result.payload)&&tag["role"].is<const char*>();
  if(!networkTagReady){tag.clear();if(networkTagFailures<2){ ++networkTagFailures; }networkTagRetryAt=millis()+(1000u<<networkTagFailures);}
}
bool GeneratorNetworkTag(const char *user) {
  if(!user||!user[0]||strlen(user)>=sizeof(networkTagKey)){ return false; }
  if(strcmp(user,networkTagKey)){GeneratorNetworkForgetTag();snprintf(networkTagKey,sizeof(networkTagKey),"%s",user);}
  if(networkTagReady){ return true; }
  if(!networkTagRequest&&int32_t(millis()-networkTagRetryAt)>=0) {
    String path="/has2.php?request=Receive&table=device&key="+NetworkEncode(user);
    networkTagRequest=NetworkSubmit(NetworkKind::Tag,path,false,GeneratorNetworkGeneration(),networkTagToken,NetworkTagResult);
    if(!networkTagRequest){ networkTagRetryAt=millis()+250; }
  }
  return false;
}
static char networkCompletion[24]={},networkCompletionDevice[32]={},networkCompletionStartState[24]={};
static uint32_t networkCompletionGeneration=0,networkCompletionRetryAt=0;
static uint32_t networkCompletionSince=0,networkCompletionWarningAt=0;
static bool networkCompletionSent=false,networkCompletionVerify=false,networkCompletionConfirmed=false,networkCompletionSuperseded=false,networkCompletionDispatched=false;
// A completion Send whose TCP connect never succeeded (connect error or the
// shared deadline expiring while still connecting) ended before the GET line
// was written, so the server cannot have applied it. Only that case is resent,
// a bounded number of times per transaction. The resend is armed only by a
// successful Verify read, taken after the failed attempt, that still shows the
// state the transaction started from: the cached state cannot be trusted while
// connects are failing (staff may have changed the server meanwhile), and a
// mutation queued through a WiFi outage would otherwise be dispatched first on
// reconnect. A queued resend is withdrawn while offline or after a short queue
// lifetime so it can never sit through an outage. It carries the original wire
// expectations, so the dispatch guard still rejects it if wires or the game
// changed. Any failure after a request byte was written remains uncertain and
// is never repeated, exactly as before.
static constexpr uint8_t kNetworkCompletionMaxAttempts=3;
static constexpr uint32_t kNetworkCompletionResendQueueLifetimeMs=5000;
static uint8_t networkCompletionAttempts=0;
static bool networkCompletionResendPending=false;   // undelivered; waiting for a fresh start-state Verify
static bool networkCompletionResendWithdrawn=false; // we cancelled the queued resend ourselves (offline/aged)
static uint32_t networkCompletionResendId=0,networkCompletionResendEnqueuedAt=0;
static int networkCompletionWireCount=-1;
static uint32_t networkCompletionWireGeneration=0;
static bool networkCompletionWireBypass=false;
static bool NetworkRequestUndelivered(const Has2BoundedTransport::Result &result) {
  return (result.error==Has2BoundedTransport::Error::Connect||result.error==Has2BoundedTransport::Error::Deadline)&&
         result.phase==Has2BoundedTransport::Phase::Connect;
}
static void NetworkCompletionResendClear() {
  networkCompletionResendPending=false;networkCompletionResendWithdrawn=false;networkCompletionResendId=0;networkCompletionResendEnqueuedAt=0;
}
static bool NetworkOwnBatteryEcho(const JsonDocument &snapshot) {
  if(!networkCompletionSent||networkCompletionConfirmed||networkCompletionSuperseded||networkCompletionGeneration!=GeneratorNetworkGeneration()||strcmp(networkCompletion,"battery_max")||
      strcmp(snapshot["device_name"]|"",networkCompletionDevice)||strcmp(snapshot["game_state"]|"","activate")||strcmp(snapshot["device_state"]|"","battery_max")){ return false; }
  bool dispatched=networkCompletionDispatched;
  portENTER_CRITICAL(&networkMux);
  for(const auto &slot:networkSlots) {
    if(slot.occupied&&slot.request.kind==NetworkKind::Completion&&slot.request.token==networkCompletionToken&&slot.request.dispatched){ dispatched=true; }
  }
  portEXIT_CRITICAL(&networkMux);
  return dispatched;
}
static bool NetworkCompletionOwns(const NetworkRequest &request) {
  return request.generation==networkCompletionGeneration&&request.token==networkCompletionToken;
}
static void NetworkCompletionObserveSnapshot() {
  if(!networkCompletionSent||networkCompletionGeneration!=GeneratorNetworkGeneration()||
      strcmp(my["device_name"]|"",networkCompletionDevice)||strcmp(my["game_state"]|"","activate")){ return; }
  const char *state=my["device_state"]|"";
  const bool wasResolved=networkCompletionConfirmed||networkCompletionSuperseded;
  // Any authoritative state other than the start state makes an undelivered
  // resend obsolete, even if the state later returns to the start value. If the
  // resend is merely pending the transaction stays unresolved (verify loop and
  // staff warning continue); if it is already queued, the queued-cancel path
  // below resets the transaction as it always did for queued completions.
  if(strcmp(state,networkCompletionStartState)){ networkCompletionResendPending=false; }
  if(!strcmp(state,networkCompletion)||(!strcmp(networkCompletion,"repaired")&&!strcmp(state,"repaired_all"))) {
    networkCompletionConfirmed=true;
  } else if(!strcmp(networkCompletion,"battery_max")&&
      (!strcmp(state,"starter_finish")||!strcmp(state,"repaired")||!strcmp(state,"repaired_all"))) {
    // A staff command or another fresh snapshot already advanced this stage.
    // Do not return battery completion to the caller and rewind local gameplay.
    networkCompletionSuperseded=true;
  }
  bool cancelQueued=false;
  portENTER_CRITICAL(&networkMux);
  if(strcmp(state,networkCompletionStartState)) {
    for(auto &slot:networkSlots) {
      if(slot.occupied&&slot.request.kind==NetworkKind::Completion&&
          slot.request.token==networkCompletionToken&&!slot.request.dispatched) {
        slot.request.cancelled=true;cancelQueued=true;
      }
    }
  }
  if(cancelQueued||(!wasResolved&&(networkCompletionConfirmed||networkCompletionSuperseded))) {
    ++networkCompletionToken;if(!networkCompletionToken){ ++networkCompletionToken; }
  }
  portEXIT_CRITICAL(&networkMux);
  if(cancelQueued&&!networkCompletionConfirmed&&!networkCompletionSuperseded) {
    networkCompletionSent=false;networkCompletionVerify=false;
    DiagnosticPrintf("[NET] queued completion cancelled after authoritative state override\r\n");
  }
  if(cancelQueued||networkCompletionConfirmed||networkCompletionSuperseded){ NetworkCompletionResendClear(); }
}
static void NetworkCompletionVerifyResult(const NetworkRequest &request,const Has2BoundedTransport::Result &result,bool cancelled) {
  if(!NetworkCompletionOwns(request)){ return; }
  networkCompletionVerify=false;networkCompletionRetryAt=millis()+2000;
  if(!NetworkOk(result,cancelled)){ return; }
  JsonDocument snapshot;
  if(deserializeJson(snapshot,result.payload)||!snapshot["device_name"].is<const char*>()||
      !snapshot["game_state"].is<const char*>()||!snapshot["device_state"].is<const char*>()||
      strcmp(snapshot["device_name"]|"",networkCompletionDevice)){ return; }
  const char *state=snapshot["device_state"]|"";
  const bool match=!strcmp(state,networkCompletion)||(!strcmp(networkCompletion,"repaired")&&!strcmp(state,"repaired_all"));
  const bool battery=!strcmp(networkCompletion,"battery_max");
  if(match&&!battery){ receiveMineOn=false; }
  // Repair confirmation remains authoritative after card removal/gauge decay.
  NetworkApplySnapshot(result,cancelled,match&&battery);
  // Fresh evidence: the server still holds the start state and the link just
  // worked. Only now may an undelivered completion be resent.
  if(networkCompletionResendPending&&!networkCompletionConfirmed&&!networkCompletionSuperseded&&
      networkCompletionGeneration==GeneratorNetworkGeneration()&&
      !strcmp(state,networkCompletionStartState)&&!strcmp(snapshot["game_state"]|"","activate")) {
    NetworkCompletionResendSubmit();
  }
}
static void NetworkCompletionSendResult(const NetworkRequest &request,const Has2BoundedTransport::Result &result,bool cancelled) {
  if(!NetworkCompletionOwns(request)){ return; }
  const bool resend=networkCompletionResendId!=0&&request.id==networkCompletionResendId;
  if(resend){ networkCompletionResendId=0;networkCompletionResendEnqueuedAt=0; }
  if(cancelled&&!request.dispatched) {
    if(resend&&networkCompletionResendWithdrawn) {
      // We withdrew our own queued resend (offline or aged in the queue). The
      // transaction is unchanged; a later fresh start-state Verify may arm again.
      networkCompletionResendWithdrawn=false;networkCompletionResendPending=true;
      DiagnosticPrintf("[NET] completion target=%s resend withdrawn before dispatch; waiting for a fresh verify\r\n",networkCompletion);
      return;
    }
    // Definite pre-dispatch cancellation has no server side effect. A fresh
    // gameplay completion check may enqueue again once its prerequisites hold.
    networkCompletionSent=false;networkCompletionVerify=false;NetworkCompletionResendClear();
    portENTER_CRITICAL(&networkMux);++networkCompletionToken;if(!networkCompletionToken){ ++networkCompletionToken; }portEXIT_CRITICAL(&networkMux);
    return;
  }
  if(request.dispatched&&networkCompletionAttempts<UINT8_MAX){ ++networkCompletionAttempts; }
  const bool undelivered=!cancelled&&request.dispatched&&NetworkRequestUndelivered(result);
  // An undelivered attempt cannot have produced a server echo, so a later
  // battery_max snapshot must be treated as a staff action, not our own.
  if(request.dispatched&&!undelivered){ networkCompletionDispatched=true; }
  if(undelivered) {
    if(networkCompletionAttempts<kNetworkCompletionMaxAttempts) {
      networkCompletionResendPending=true;
      DiagnosticPrintf("[NET] completion target=%s undelivered (connect failed before any request byte); attempt %u/%u, resend armed by the next start-state verify\r\n",
        networkCompletion,unsigned(networkCompletionAttempts),unsigned(kNetworkCompletionMaxAttempts));
    } else {
      networkCompletionResendPending=false;
      DiagnosticPrintf("[NET] completion target=%s undelivered; resend budget exhausted, read-only verification continues\r\n",networkCompletion);
    }
  }
  // HTTP 200 alone is not proof of an applied mutation. Always read actual state.
  networkCompletionRetryAt=millis();
}
static void NetworkCompletionResendSubmit() {
  // Called from a successful Verify result only. Same transaction and token;
  // the original wire expectations let the dispatch guard reject wire changes.
  String path="/has2.php?request=Send&table=device&key="+NetworkEncode(networkCompletionDevice)+"&column=device_state&value="+NetworkEncode(networkCompletion);
  const uint32_t id=NetworkSubmit(NetworkKind::Completion,path,true,networkCompletionGeneration,networkCompletionToken,NetworkCompletionSendResult,
      networkCompletionWireCount,networkCompletionWireGeneration,networkCompletionWireBypass);
  if(id) {
    networkCompletionResendPending=false;networkCompletionResendWithdrawn=false;
    networkCompletionResendId=id;networkCompletionResendEnqueuedAt=millis();
    DiagnosticPrintf("[NET] completion target=%s resend enqueued after fresh start-state verify (attempts used %u/%u)\r\n",
      networkCompletion,unsigned(networkCompletionAttempts),unsigned(kNetworkCompletionMaxAttempts));
  }
  // Queue full: stay pending; the next successful Verify arms it again.
}
static void NetworkCompletionResendService() {
  if(!networkCompletionResendId||networkCompletionResendWithdrawn){ return; }
  // Never let a queued resend wait through an outage or a long queue: by the
  // time it would dispatch, the server state it was based on may be stale.
  const bool offline=WiFi.status()!=WL_CONNECTED;
  const bool aged=uint32_t(millis()-networkCompletionResendEnqueuedAt)>kNetworkCompletionResendQueueLifetimeMs;
  if(!offline&&!aged){ return; }
  bool withdrawn=false;
  portENTER_CRITICAL(&networkMux);
  for(auto &slot:networkSlots) {
    if(slot.occupied&&!slot.active&&slot.request.id==networkCompletionResendId&&!slot.request.cancelled){ slot.request.cancelled=true;withdrawn=true; }
  }
  portEXIT_CRITICAL(&networkMux);
  if(withdrawn) {
    networkCompletionResendWithdrawn=true;
    DiagnosticPrintf("[NET] completion target=%s queued resend withdrawn (%s)\r\n",networkCompletion,offline?"offline":"queue lifetime exceeded");
  }
}
static void NetworkCompletionService() {
  if(!networkCompletionSent||networkCompletionConfirmed||networkCompletionSuperseded||
      networkCompletionGeneration!=GeneratorNetworkGeneration()){ return; }
  if(uint32_t(millis()-networkCompletionSince)>=30000&&
      uint32_t(millis()-networkCompletionWarningAt)>=30000) {
    networkCompletionWarningAt=millis();
    DiagnosticPrintf("[NET] completion target=%s unconfirmed for %lu ms; Send attempts used %u/%u (repeated only while undelivered at connect); staff recovery required if state remains unchanged\r\n",
      networkCompletion,(unsigned long)(millis()-networkCompletionSince),unsigned(networkCompletionAttempts),unsigned(kNetworkCompletionMaxAttempts));
  }
  NetworkCompletionResendService();
  if(networkCompletionVerify||int32_t(millis()-networkCompletionRetryAt)<0){ return; }
  String path="/has2.php?request=ReceiveMine&table=device&mac="+NetworkEncode(has2wifi.GetMacAddress().c_str());
  networkCompletionVerify=NetworkSubmit(NetworkKind::Verify,path,false,networkCompletionGeneration,networkCompletionToken,NetworkCompletionVerifyResult)!=0;
  if(!networkCompletionVerify){ networkCompletionRetryAt=millis()+250; }
}
bool GeneratorNetworkComplete(const char *state) {
  if(!state||!networkBootReady||(strcmp(state,"battery_max")&&strcmp(state,"repaired"))){ return false; }
  const uint32_t generation=GeneratorNetworkGeneration();
  if(generation!=networkCompletionGeneration||strcmp(state,networkCompletion)) {
    // Do not replace an unresolved transaction just because the caller changed.
    // A fresh authoritative snapshot can confirm/supersede the preceding stage.
    if(generation==networkCompletionGeneration&&networkCompletionSent&&
        !networkCompletionConfirmed&&!networkCompletionSuperseded){ return false; }
    portENTER_CRITICAL(&networkMux);++networkCompletionToken;if(!networkCompletionToken){ ++networkCompletionToken; }portEXIT_CRITICAL(&networkMux);
    snprintf(networkCompletion,sizeof(networkCompletion),"%s",state);
    snprintf(networkCompletionDevice,sizeof(networkCompletionDevice),"%s",my["device_name"]|"");
    snprintf(networkCompletionStartState,sizeof(networkCompletionStartState),"%s",my["device_state"]|"");
    networkCompletionGeneration=generation;
    networkCompletionSent=false;networkCompletionVerify=false;networkCompletionConfirmed=false;networkCompletionSuperseded=false;networkCompletionDispatched=false;
    networkCompletionRetryAt=0;
    networkCompletionAttempts=0;NetworkCompletionResendClear();
  }
  if(networkCompletionConfirmed){ return true; }
  if(networkCompletionSuperseded){ return false; }
  if(!networkCompletionSent) {
    const GeneratorWireState wire=WireSnapshot();
    const bool bypass=WireServerCompleted();
    if(!bypass&&!wire.qualified(millis())){ return false; }
    String path="/has2.php?request=Send&table=device&key="+NetworkEncode(networkCompletionDevice)+"&column=device_state&value="+NetworkEncode(state);
    if(NetworkSubmit(NetworkKind::Completion,path,true,generation,networkCompletionToken,NetworkCompletionSendResult,wire.stable(),wire.generation(),bypass)) {
      snprintf(networkCompletionStartState,sizeof(networkCompletionStartState),"%s",my["device_state"]|"");
      networkCompletionWireCount=wire.stable();networkCompletionWireGeneration=wire.generation();networkCompletionWireBypass=bypass;
      networkCompletionAttempts=0;NetworkCompletionResendClear();
      networkCompletionSent=true;networkCompletionRetryAt=millis()+2500;
      networkCompletionSince=millis();networkCompletionWarningAt=millis();
    }
  }
  NetworkCompletionService();
  return false;
}
void GeneratorNetworkRequestMaintenance(bool legacy) {
  if(networkMaintenance){ return; }
  if(!legacy&&NetworkOtaAttempts()>=kNetworkOtaMaxAttempts) {
    // Each attempt costs a restart; stop until staff set another state.
    DiagnosticPrintf("[NET] GitHub OTA not retried: %lu attempts without update; set another device_state to reset\r\n",
      (unsigned long)NetworkOtaAttempts());
    return;
  }
  networkMaintenance=true;networkMaintenanceLegacy=legacy;networkMaintenanceStarted=millis();
  has1blackbox::blackbox.setUploadAllowed(false);
  portENTER_CRITICAL(&networkMux);
  for(auto &slot:networkSlots)if(slot.occupied&&!slot.active){ slot.request.cancelled=true; }
  portEXIT_CRITICAL(&networkMux);
  GeneratorNetworkInvalidateGame();EncoderDetach();ptrCurrentMode=WaitFunc;
  DiagnosticPrintf("[NET] maintenance requested; draining bounded network work before OTA\r\n");
}
// The core's mbedTLS has fixed 16 KiB in/out record buffers: one GitHub HTTPS
// request needs ~45 KiB, two ~17 KiB blocks contiguous. With BLE running ~48 KiB
// was free and every GitHub request failed at connect (HTTP -1). Stopping the
// controller gave TLS ~20 KiB, but Update.begin() could then not get its 4 KiB
// buffer once the download started, so its static memory is released as well.
// BLE comes back only at boot: the caller restarts if OTA returns. The local HTTP
// update needs no TLS and keeps its proven path without any of this.
static esp_timer_handle_t networkOtaGuard=nullptr;
static void NetworkOtaGuardExpired(void*) { esp_restart(); }
static void NetworkGithubOta() {
  // Counted first, so a reset anywhere below (BLE release included) is bounded.
  const uint32_t attempt=NetworkOtaAttempts()+1;
  NetworkOtaAttemptsSet(attempt);
  const uint32_t heap=ESP.getFreeHeap(),largest=ESP.getMaxAllocHeap();
  Has1BleBeacon::suspend();
  const bool released=Has1BleBeacon::releaseMemory();
  DiagnosticPrintf("[NET] OTA memory heap=%lu largest=%lu; BLE off heap=%lu largest=%lu released=%u bb_idle=%u\r\n",
    (unsigned long)heap,(unsigned long)largest,(unsigned long)ESP.getFreeHeap(),
    (unsigned long)ESP.getMaxAllocHeap(),unsigned(released),unsigned(has1blackbox::blackbox.uploadIdle()));
  // The loop task is off its watchdog here and BLE is gone for this boot; a
  // download stalled past SecureOTA's own timeouts must still end in a restart.
  const esp_timer_create_args_t guard={NetworkOtaGuardExpired,nullptr,ESP_TIMER_TASK,"ota-guard",false};
  if(!networkOtaGuard){ esp_timer_create(&guard,&networkOtaGuard); }
  if(networkOtaGuard){ esp_timer_start_once(networkOtaGuard,300000000ull); }
  ota.check(); // Restarts after a verified update; returns on failure or skip.
  if(networkOtaGuard){ esp_timer_stop(networkOtaGuard); }
  DiagnosticPrintf("[NET] OTA returned without update; restart pending (attempt %lu of %lu)\r\n",
    (unsigned long)attempt,(unsigned long)kNetworkOtaMaxAttempts);
}
void GeneratorNetworkBegin() {
  if(networkTask){ return; }
  const bool prior=GeneratorOfflineRecovery::boot(networkRecoveryHistory,esp_reset_reason()==ESP_RST_SW);
  const int reason=esp_reset_reason();
  if(reason!=ESP_RST_SW&&reason!=ESP_RST_PANIC&&reason!=ESP_RST_INT_WDT&&
     reason!=ESP_RST_TASK_WDT&&reason!=ESP_RST_WDT){ NetworkOtaAttemptsSet(0); }
  const String mac=has2wifi.GetMacAddress();
  for(size_t i=0;i<mac.length();++i){networkRecoverySalt=networkRecoverySalt*33u+uint8_t(mac[i]);}
  if(prior) {
    DiagnosticPrintf("[NET] prior wifi-offline recovery restart: attempt=%lu outage_ms=%lu\r\n",
      (unsigned long)networkRecoveryHistory.attempts,(unsigned long)networkRecoveryHistory.outageMs);
    BlackBoxOfflineRecovery(5,networkRecoveryHistory.outageMs,networkRecoveryHistory.attempts,0);
  }
  const BaseType_t created=xTaskCreatePinnedToCore(NetworkWorker,"generator-net",8192,nullptr,1,&networkTask,0);
  if(created!=pdPASS){networkTask=nullptr;DiagnosticPrintf("[NET] worker creation failed; gameplay remains unconfirmed\r\n");}
}
// Called only from main, after mailbox callbacks have settled transaction state.
// The dispatch fence and worker selection share networkMux. No task deletion,
// driver buffer disposal, Wi-Fi mode toggle or transport replay is introduced.
static bool NetworkOfflineRecoveryService() {
  const uint32_t now=millis();
  const bool online=WiFi.status()==WL_CONNECTED;
  const bool wasOffline=networkRecoveryPolicy.offline;
  if(networkRecoveryPolicy.sample(now,online,networkRecoveryHistory)) {
    BlackBoxOfflineRecovery(4,0,0,0);
    DiagnosticPrintf("[NET] wifi-offline restart backoff reset after 10 minutes of healthy link/server progress\r\n");
  }
  if(!online&&!wasOffline) {
    networkRecoveryBlockedLogged=false;
    BlackBoxOfflineRecovery(1,0,networkRecoveryHistory.attempts,
      GeneratorOfflineRecovery::delayMs(networkRecoveryHistory.attempts,networkRecoverySalt,networkRecoveryPolicy.hadConnection));
  }
  if(!networkRecoveryPolicy.due(now,networkRecoveryHistory,networkRecoverySalt)){return false;}
  uint32_t blocked=0;
  if(networkMaintenance){blocked|=1;}
  // A possibly delivered completion must retain its read-only confirmation
  // state. Restarting could cause gameplay to issue the same mutation again.
  if(networkCompletionSent&&networkCompletionDispatched&&!networkCompletionConfirmed&&
      !networkCompletionSuperseded&&networkCompletionGeneration==GeneratorNetworkGeneration()){blocked|=2;}
  portENTER_CRITICAL(&networkMux);
  if(!networkTask||networkDriverFault){blocked|=4;}
  if(networkMailboxReady){blocked|=8;}
  for(const auto &slot:networkSlots)if(slot.occupied&&slot.active){blocked|=8;}
  if(!blocked){networkRecoveryFence=true;}
  portEXIT_CRITICAL(&networkMux);
  if(blocked) {
    if(!networkRecoveryBlockedLogged||blocked!=networkRecoveryBlockedReason||uint32_t(now-networkRecoveryLastLog)>=30000) {
      networkRecoveryBlockedLogged=true;networkRecoveryLastLog=now;networkRecoveryBlockedReason=blocked;
      BlackBoxOfflineRecovery(2,networkRecoveryPolicy.elapsed(now),networkRecoveryHistory.attempts,blocked);
      DiagnosticPrintf("[NET] wifi-offline restart deferred: age_ms=%lu blockers=%lu (1=OTA 2=uncertain_completion 4=worker_fault 8=in_flight)\r\n",
        (unsigned long)networkRecoveryPolicy.elapsed(now),(unsigned long)blocked);
    }
    return false;
  }
  // Recheck current association after the fence, not a stale worker snapshot.
  if(WiFi.status()==WL_CONNECTED) {
    portENTER_CRITICAL(&networkMux);networkRecoveryFence=false;portEXIT_CRITICAL(&networkMux);
    networkRecoveryPolicy.sample(millis(),true,networkRecoveryHistory);return false;
  }
  GeneratorOfflineRecovery::requested(networkRecoveryHistory,networkRecoveryPolicy.elapsed(now));
  has1blackbox::blackbox.setUploadAllowed(false);
  BlackBoxOfflineRecovery(3,networkRecoveryHistory.outageMs,networkRecoveryHistory.attempts,0);
  DiagnosticPrintf("[NET] wifi-offline recovery restart: attempt=%lu outage_ms=%lu\r\n",
    (unsigned long)networkRecoveryHistory.attempts,(unsigned long)networkRecoveryHistory.outageMs);
  // RTC intent is written before restart. BlackBox delivery is best effort;
  // next boot re-emits the retained intent. No blocking flush on this path.
  ESP.restart();return true;
}
void GeneratorNetworkLoop() {
  portENTER_CRITICAL(&networkMux);
  const bool ready=networkMailboxReady;const int index=networkMailboxSlot;
  NetworkRequest request={};if(ready&&index>=0){ request=networkSlots[index].request; }
  const NetworkDiscard mailboxDiscard=networkMailboxDiscard;
  const uint32_t workerStackFree=networkWorkerStackFree;
  unsigned queueDepth=0;for(const auto &slot:networkSlots)if(slot.occupied){ ++queueDepth; }
  const uint32_t activeId=networkWorkerActiveId,activeStart=networkWorkerStarted,progressAt=networkWorkerProgressAt;
  const bool scanPending=networkScanPending,driverFault=networkDriverFault;
  const Has2BoundedTransport::Phase phase=networkWorkerPhase;
  portEXIT_CRITICAL(&networkMux);
  BlackBoxRecoveryState(queueDepth,ready,scanPending,driverFault,networkMaintenance);
  BlackBoxNetwork(queueDepth!=0 || ready || scanPending || driverFault,networkMaintenance,
      activeId,int(phase),activeId?millis()-activeStart:0,activeId?millis()-progressAt:0,workerStackFree);
  static uint32_t seenActive=0,lastActiveLog=0,scanSince=0,seenDisconnect=0;
  const HAS2_WifiDisconnectDiagnostic disconnect=has2wifi.GetDisconnectDiagnostic();
  if(disconnect.sequence!=seenDisconnect) {
    DiagnosticPrintf("[NET] WiFi disconnected reason=%u at_ms=%lu events=%lu (may include intentional disconnect)\r\n",unsigned(disconnect.reason),(unsigned long)disconnect.atMs,(unsigned long)(disconnect.sequence-seenDisconnect));
    seenDisconnect=disconnect.sequence;
  }
  static Has2BoundedTransport::Phase seenPhase=Has2BoundedTransport::Phase::Done;
  static bool faultLogged=false;
  if(driverFault&&!faultLogged){faultLogged=true;DiagnosticPoint(GeneratorDiagnostic::Has2Http,-202,0);DiagnosticPrintf("[NET] WiFi scan completion lost; worker watchdog fail-stop\r\n");}
  if(activeId&&!ready&&(activeId!=seenActive||phase!=seenPhase||uint32_t(millis()-lastActiveLog)>=5000)) {
    seenActive=activeId;seenPhase=phase;lastActiveLog=millis();
    DiagnosticPoint(GeneratorDiagnostic::Has2Http,-201,(activeId<<8)|uint32_t(phase));
    DiagnosticPrintf("[NET] active id=%lu phase=%u elapsed=%lu worker_progress_age=%lu\r\n",(unsigned long)activeId,unsigned(phase),(unsigned long)(millis()-activeStart),(unsigned long)(millis()-progressAt));
  }
  if(!scanPending){ scanSince=0; }
  else if(!scanSince){ scanSince=millis(); }
  else if(uint32_t(millis()-scanSince)>=10000){scanSince=millis();DiagnosticPrintf("[NET] WiFi scan completion missing; recovery waiting\r\n");}
  if(ready&&index>=0) {
    NetworkDiscard discard=mailboxDiscard;
    if(discard==NetworkDiscard::None){ discard=NetworkDiscardReason(request); }
    if(discard==NetworkDiscard::None&&!request.mutation&&uint32_t(millis()-request.started)>kNetworkReadLifetime){ discard=NetworkDiscard::ResultExpired; }
    const bool stale=discard!=NetworkDiscard::None;
    BlackBoxHttpDetail(request,networkMailbox,stale,int(discard));
    BlackBoxHttp(int(request.kind),int(networkMailbox.error),networkMailbox.status,
        networkMailbox.timings.totalMs,stale);
    DiagnosticPrintf("[NET] id=%lu gen=%lu current_gen=%lu depth=%u discard=%s worker_stack_free=%lu\r\n",(unsigned long)request.id,(unsigned long)request.generation,(unsigned long)GeneratorNetworkGeneration(),queueDepth,NetworkDiscardName(discard),(unsigned long)workerStackFree);
    DiagnosticPrintf("[NET] id=%lu kind=%u queue_ms=%lu status=%d error=%u phase=%u errno=%d connect=%lu write=%lu first=%lu header=%lu body=%lu close=%lu total=%lu stale=%u\r\n",
      (unsigned long)request.id,unsigned(request.kind),(unsigned long)(request.started-request.enqueued),networkMailbox.status,
      unsigned(networkMailbox.error),unsigned(networkMailbox.phase),networkMailbox.socketError,
      (unsigned long)networkMailbox.timings.connectMs,(unsigned long)networkMailbox.timings.writeMs,(unsigned long)networkMailbox.timings.firstByteMs,
      (unsigned long)networkMailbox.timings.headerMs,(unsigned long)networkMailbox.timings.bodyMs,(unsigned long)networkMailbox.timings.closeMs,(unsigned long)networkMailbox.timings.totalMs,stale);
    // Free queue capacity before callbacks enqueue follow-up work. Worker remains
    // blocked by mailboxReady until the callback no longer uses its payload.
    portENTER_CRITICAL(&networkMux);networkSlots[index].occupied=false;networkSlots[index].active=false;portEXIT_CRITICAL(&networkMux);
    if(request.callback){ request.callback(request,networkMailbox,stale); }
    networkMailbox.payload="";
    portENTER_CRITICAL(&networkMux);networkMailboxReady=false;networkMailboxSlot=-1;networkWorkerActiveId=0;portEXIT_CRITICAL(&networkMux);
  }
  NetworkCompletionService();
  if(NetworkOfflineRecoveryService()){return;}
  const bool online=WiFi.status()==WL_CONNECTED;
  if(!networkMaintenance&&online&&!networkWasConnected){GeneratorNetworkRefresh();GeneratorNetworkRequestPoll();}
  networkWasConnected=online;
  if(!networkMaintenance&&online&&networkRefreshPending&&!networkRefreshInFlight&&int32_t(millis()-networkRefreshAt)>=0) {
    if(!NetworkQueueRefresh()){ networkRefreshAt=millis()+250; }
  }
  if(!networkMaintenance&&networkBootReady&&!networkVersionSent){ networkVersionSent=GeneratorNetworkSend(my["device_name"]|"","esp_version",String(FIRMWARE_VER).c_str()); }
  if(networkMaintenance) {
    portENTER_CRITICAL(&networkMux);
    bool busy=networkMailboxReady;for(const auto &slot:networkSlots)if(slot.occupied){ busy=true; }
    portEXIT_CRITICAL(&networkMux);
    if(networkOtaRestartPending) {
      // Released BLE memory returns only through boot. Let the OTA callback's
      // state report leave first, for at most ten seconds.
      if(!busy||uint32_t(millis()-networkOtaRestartSince)>=10000) {
        DiagnosticPrintf("[NET] restarting after GitHub OTA without update; BLE returns at boot\r\n");
        BlackBoxOtaRestart(NetworkOtaAttempts());
        networkOtaRestartPending=false;networkMaintenance=false;
        ESP.restart();
      }
      return;
    }
    // GitHub OTA also waits for the withdrawn MQTT session to free its client,
    // task and buffers; a stuck SDK stop gets 15 seconds, then OTA proceeds.
    if(!networkMaintenanceLegacy&&!has1blackbox::blackbox.uploadIdle()&&
       uint32_t(millis()-networkMaintenanceStarted)<15000){ busy=true; }
    if(!online&&uint32_t(millis()-networkMaintenanceStarted)>=10000) {
      networkMaintenance=false;GeneratorNetworkRefreshCycle();
      DiagnosticPrintf("[NET] OTA maintenance aborted: WiFi offline for 10 seconds; request again after recovery\r\n");
    } else if(!busy&&online) {
      has1blackbox::blackbox.setPaused(true);
      esp_task_wdt_delete(nullptr);
      if(networkMaintenanceLegacy){ has2wifi.FirmwareUpdate(my["device_type"]|"generator","172.30.1.43"); }
      else NetworkGithubOta();
      esp_task_wdt_add(nullptr);
      has1blackbox::blackbox.setPaused(false);
      if(networkMaintenanceLegacy){ networkMaintenance=false;GeneratorNetworkRefreshCycle(); }
      else { networkOtaRestartPending=true;networkOtaRestartSince=millis(); }
    }
  }

}
