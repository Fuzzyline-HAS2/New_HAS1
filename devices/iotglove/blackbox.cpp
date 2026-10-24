#include "blackbox.h"
#include <BlackBoxEvidence.h>
#include "secrets.h"
#include "library_and_pin.h"
#include "wifi_client.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
namespace iotglove {
using has1blackbox::blackbox;
using has1blackbox::EventType;
static uint32_t adcAt=0,adcMin=UINT32_MAX,adcCount=0,batteryAt=0;
static bool adcKnown=false,batteryKnown=false;
void blackBoxAdcSample(uint32_t now,uint32_t adcMv) {
  adcKnown=true;adcAt=now;if(adcMv<adcMin)adcMin=adcMv;
  if(adcCount<UINT32_MAX)++adcCount;
}
void blackBoxBatteryVoltage(uint32_t now) {batteryKnown=true;batteryAt=now;}
void blackBoxOutput(uint32_t now,bool motor,int lit,int brightness) {
  static bool known=false,priorMotor=false;
  static int priorLit=0,priorBrightness=0;
  static uint32_t changedAt=0,suppressed=0;
  if(known && motor==priorMotor && lit==priorLit && brightness==priorBrightness)return;
  priorMotor=motor;priorLit=lit;priorBrightness=brightness;
  if(known && uint32_t(now-changedAt)<500) {++suppressed;return;}
  known=true;changedAt=now;
  blackbox.log(EventType::State,motor,lit,brightness,suppressed,"output.applied");
  suppressed=0;
}
static portMUX_TYPE uploadPolicyMux=portMUX_INITIALIZER_UNLOCKED;
static bool mainAllows=false,networkIdle=false;
static void updateUpload(bool mainProducer,bool value) {
  // Serialize the two producers with the atomic-only library setter. Otherwise
  // a stale main read could re-enable upload after the worker starts HTTP.
  portENTER_CRITICAL(&uploadPolicyMux);
  if(mainProducer) mainAllows=value;else networkIdle=value;
  blackbox.setUploadAllowed(mainAllows && networkIdle);
  portEXIT_CRITICAL(&uploadPolicyMux);
}
void blackBoxBegin(int firmware,int partition) {
  static char version[16]; snprintf(version,sizeof(version),"%d",firmware);
  blackbox.begin({"iotglove",version,
      IOTGLOVE_BLACKBOX_BROKER_URI,IOTGLOVE_BLACKBOX_ENABLED != 0,
      IOTGLOVE_BLACKBOX_MQTT_USERNAME,IOTGLOVE_BLACKBOX_MQTT_PASSWORD});
  blackbox.log(EventType::Boot,esp_reset_reason(),firmware,partition,IOTGLOVE_TRAINING,"device.boot");
}
void blackBoxOperation(int phase) {
  const bool idle=phase==int(NetworkPhase::Idle) || phase==int(NetworkPhase::Backoff);
  updateUpload(false,idle);
  static BlackBoxRateGate gate;
  if(!idle && gate.allow(millis(),60000))
    blackbox.log(EventType::Progress,phase,0,0,0,"network.begin");
}
void blackBoxHttp(int error,int status,uint32_t duration,bool valid) {
  static BlackBoxRateGate gate;
  static has1blackbox::EvidenceGate failures;
  if(failures.observe(millis(),!valid,error,0,status))
    blackbox.log(EventType::HttpSummary,error,status,duration,valid,
        valid?"primary.recovered":"primary.failure");
  static uint32_t total=0,invalid=0,maxMs=0;
  ++total; if(!valid) ++invalid; if(duration>maxMs) maxMs=duration;
  if(!gate.allow(millis())) return;
  blackbox.log(EventType::HttpSummary,error,status,duration,valid,"primary.result");
  blackbox.log(EventType::HttpSummary,total,invalid,maxMs,0,"primary.counts");
  total=invalid=maxMs=0;
}
void blackBoxLoop(uint32_t now,int phase,int role,int deviceState,bool known,bool ota,
                  bool motor,int lit,int adc,int battery,bool peerOnline) {
  updateUpload(true,blackBoxUploadAllowed(known,phase==int(Phase::Active),phase==int(Phase::Photo),
      WiFi.status()==WL_CONNECTED,false,ota));
  static BlackBoxSummary summary;
  static int lastPhase=-1,lastRole=-1,lastState=-1;
  static bool lastKnown=false;
  static BlackBoxRateGate stateGate;
  if((known!=lastKnown || phase!=lastPhase || role!=lastRole || deviceState!=lastState) && stateGate.allow(now,1000)) {
    blackbox.log(EventType::State,known?phase:-1,known?role:-1,known?deviceState:-1,0,"game");
    lastPhase=phase;lastRole=role;lastState=deviceState;lastKnown=known;
  }
  if(!summary.sample(now)) return;
  blackbox.log(EventType::Progress,summary.maxGap,summary.loops,0,0,"main.summary");
  blackbox.log(EventType::Health,ESP.getFreeHeap(),ESP.getMinFreeHeap(),
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),uxTaskGetStackHighWaterMark(nullptr),"memory.main");
  blackbox.log(EventType::State,motor,lit,adc,battery,"output.adc_mv.batt_mv");
  blackbox.log(EventType::Health,adcMin==UINT32_MAX?has1blackbox::Unknown:int32_t(adcMin),
      adcKnown?int32_t(now-adcAt):has1blackbox::Unknown,adcCount,
      batteryKnown?int32_t(now-batteryAt):has1blackbox::Unknown,"adc.min.age.n.batt_age");
  adcMin=UINT32_MAX;adcCount=0;
  blackbox.log(EventType::Peer,peerOnline,0,0,0,"beetle.online");
  NetworkDiagnostics d;
  if(networkDiagnostics(d)) {
    blackbox.log(EventType::Progress,int(d.phase),now-d.startedAtMs,now-d.progressAtMs,d.sequence,"network.summary");
    blackbox.log(EventType::WifiSummary,d.wifiConnected,d.wifiConnected?d.rssi:INT32_MIN,
        d.wifiConnectFailures,d.wifiLastLossReason,"wifi");
    blackbox.log(EventType::Recovery,d.primaryTransportFailures,d.forcedReconnects,
        d.reconnectCooldownMs,d.workerStackHighWater,"recovery.worker_stack");
  }
  summary.emitted(now);
}
void blackBoxBeforeRestart(int reason,int attempts) {
  updateUpload(true,false);
  // Existing recovery deadline already expired: enqueue only. Do not postpone
  // recovery for a best-effort flush or feed the game watchdog from logging.
  blackbox.log(EventType::Recovery,reason,attempts,0,0,"restart.request");
}
}
