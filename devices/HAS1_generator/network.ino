// One bounded transport owner. Queue metadata is fixed; at most one response
// body exists between the worker and main. Only main owns game/JSON state.
#include "generator_network.h"
#include <has2_bounded_transport.h>
#include <esp_task_wdt.h>

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
static bool networkWasConnected=false;
static uint32_t networkRefreshAt=0;
static char networkIdentity[32]={},networkGame[32]={};

static char networkTagKey[16]={};
static uint32_t networkTagToken=1,networkTagRequest=0,networkTagRetryAt=0;
static bool networkTagReady=false;
static uint8_t networkTagFailures=0;
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
    void (*callback)(const NetworkRequest &,const Has2BoundedTransport::Result &,bool),int expectedWireCount=-1) {
  if (networkMaintenance&&kind!=NetworkKind::Report){ return 0; }
  if (path.length()>=sizeof(NetworkRequest::path)) { return 0; }
  NetworkRequest request={};
  memcpy(request.path,path.c_str(),path.length()+1);
  request.kind=kind;request.mutation=mutation;request.generation=generation;request.token=token;
  request.enqueued=millis();request.callback=callback;request.expectedWireCount=expectedWireCount;
  portENTER_CRITICAL(&networkMux);
  unsigned used=0;for (const auto &slot:networkSlots) if(slot.occupied){ ++used; }
  // Leave capacity for heartbeat/state repair when optional reports fill the queue.
  const bool optional=kind==NetworkKind::Contribution||kind==NetworkKind::Report;
  if (networkTask&&!(optional&&used>=kNetworkSlots-2)&&!(kind!=NetworkKind::Poll&&used>=kNetworkSlots-1)) {
    for(auto &slot:networkSlots) if(!slot.occupied) {
      request.id=networkNextId++;if(!networkNextId){ ++networkNextId; }
      slot.request=request;slot.occupied=true;slot.active=false;break;
    }
  }
  portEXIT_CRITICAL(&networkMux);
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
    if (!mailboxBusy) {
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
  if(!has2wifi.ApplyNetworkSnapshot("ReceiveMine",result.payload)){ return false; }
  NetworkIdentityChanged();networkBootReady=true;
  if(!unchangedState||networkResetCyclePending) {
    // Close before a same-game mode override can reset the encoder. A new game
    // generation deliberately discards the prior session in ContribEnd.
    ContribEnd(starterContribLastCnt);starterLastTagState=false;
  }
  if(localCompletion){ receiveMineOn=true; }
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
  if(!strcmp(my["device_state"] | "", "update")){ GeneratorNetworkRequestMaintenance(true); }
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
  if(!NetworkOk(result,cancelled)||!has2wifi.ApplyNetworkSnapshot("Loop",result.payload)){NetworkFinishPoll(false);return;}
  if((int)shift_machine["watchdog"]>=1){ networkWatchdogRequested=true; }
  else if(networkWatchdogRequested&&!networkWatchdogPending) {
    DiagnosticPrintf("[NET] explicit server reboot clear confirmed by poll\r\n");ESP.restart();
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
#if !GENERATOR_ALLOW_UNFENCED_COMPLETION
  static uint32_t warned=0;
  if(warned!=GeneratorNetworkGeneration()){warned=GeneratorNetworkGeneration();DiagnosticPrintf("[NET] CONTRIBUTION BLOCKED: server epoch-checked event contract required\r\n");}
  return false;
#endif
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
static char networkCompletion[24]={};
static uint32_t networkCompletionGeneration=0,networkCompletionRetryAt=0;
static bool networkCompletionSent=false,networkCompletionVerify=false,networkCompletionConfirmed=false;
static void NetworkCompletionVerifyResult(const NetworkRequest &request,const Has2BoundedTransport::Result &result,bool cancelled) {
  if(request.generation!=networkCompletionGeneration){ return; }
  networkCompletionVerify=false;networkCompletionRetryAt=millis()+2000;
  if(!NetworkOk(result,cancelled)){ return; }
  JsonDocument snapshot;if(deserializeJson(snapshot,result.payload)){ return; }
  const char *state=snapshot["device_state"]|"";
  const bool match=!strcmp(state,networkCompletion)||(!strcmp(networkCompletion,"repaired")&&!strcmp(state,"repaired_all"));
  const bool battery=!strcmp(networkCompletion,"battery_max");
  if(match&&!battery){ receiveMineOn=false; }
  // Repair confirmation is authoritative even if the card was removed or its
  // gauge decayed while waiting. Apply its normal DataChanged transition now.
  if(NetworkApplySnapshot(result,cancelled,match&&battery)&&match&&request.generation==GeneratorNetworkGeneration()){ networkCompletionConfirmed=true; }
}
static void NetworkCompletionSendResult(const NetworkRequest &request,const Has2BoundedTransport::Result &,bool) {
  if(request.generation!=networkCompletionGeneration){ return; }
  // Never repeat an uncertain legacy mutation. Verify actual state instead.
  networkCompletionRetryAt=millis();
}
static void NetworkCompletionService() {
#if GENERATOR_ALLOW_UNFENCED_COMPLETION
  if(!networkCompletionSent||networkCompletionConfirmed||networkCompletionVerify||
      networkCompletionGeneration!=GeneratorNetworkGeneration()||int32_t(millis()-networkCompletionRetryAt)<0){ return; }
  String path="/has2.php?request=ReceiveMine&table=device&mac="+NetworkEncode(has2wifi.GetMacAddress().c_str());
  networkCompletionVerify=NetworkSubmit(NetworkKind::Verify,path,false,networkCompletionGeneration,0,NetworkCompletionVerifyResult)!=0;
  if(!networkCompletionVerify){ networkCompletionRetryAt=millis()+250; }
#endif
}
bool GeneratorNetworkComplete(const char *state) {
  if(!state||!networkBootReady){ return false; }
  const uint32_t generation=GeneratorNetworkGeneration();
  if(generation!=networkCompletionGeneration||strcmp(state,networkCompletion)) {
    snprintf(networkCompletion,sizeof(networkCompletion),"%s",state);networkCompletionGeneration=generation;
    networkCompletionSent=false;networkCompletionVerify=false;networkCompletionConfirmed=false;networkCompletionRetryAt=0;
  }
  if(networkCompletionConfirmed){ return true; }
#if !GENERATOR_ALLOW_UNFENCED_COMPLETION
  static uint32_t loggedGeneration=0;
  if(loggedGeneration!=generation){loggedGeneration=generation;DiagnosticPrintf("[NET] COMPLETION BLOCKED: server epoch-checked completion contract required; deployment gate active\r\n");}
  return false;
#else
  if(!networkCompletionSent) {
    String path="/has2.php?request=Send&table=device&key="+NetworkEncode(my["device_name"]|"")+"&column=device_state&value="+NetworkEncode(state);
    if(NetworkSubmit(NetworkKind::Completion,path,true,generation,0,NetworkCompletionSendResult)) {
      networkCompletionSent=true;networkCompletionRetryAt=millis()+2500;
    }
  }
  NetworkCompletionService();
  return false;
#endif
}
void GeneratorNetworkRequestMaintenance(bool legacy) {
  if(networkMaintenance){ return; }
  networkMaintenance=true;networkMaintenanceLegacy=legacy;networkMaintenanceStarted=millis();
  portENTER_CRITICAL(&networkMux);
  for(auto &slot:networkSlots)if(slot.occupied&&!slot.active){ slot.request.cancelled=true; }
  portEXIT_CRITICAL(&networkMux);
  GeneratorNetworkInvalidateGame();EncoderDetach();ptrCurrentMode=WaitFunc;
  DiagnosticPrintf("[NET] maintenance requested; draining bounded network work before OTA\r\n");
}
void GeneratorNetworkBegin() {
  if(networkTask){ return; }
  const BaseType_t created=xTaskCreatePinnedToCore(NetworkWorker,"generator-net",8192,nullptr,1,&networkTask,0);
  if(created!=pdPASS){networkTask=nullptr;DiagnosticPrintf("[NET] worker creation failed; gameplay remains unconfirmed\r\n");}
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
    if(!online&&uint32_t(millis()-networkMaintenanceStarted)>=10000) {
      networkMaintenance=false;GeneratorNetworkRefreshCycle();
      DiagnosticPrintf("[NET] OTA maintenance aborted: WiFi offline for 10 seconds; request again after recovery\r\n");
    } else if(!busy&&online) {
      esp_task_wdt_delete(nullptr);
      if(networkMaintenanceLegacy){ has2wifi.FirmwareUpdate(my["device_type"]|"generator","172.30.1.43"); }
      else ota.check();
      esp_task_wdt_add(nullptr);
      networkMaintenance=false;GeneratorNetworkRefreshCycle();
    }
  }

}
