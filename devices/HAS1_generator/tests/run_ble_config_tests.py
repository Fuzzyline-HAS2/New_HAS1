#!/usr/bin/env python3
"""Compile both generator BLE modes and the actual blackbox boot configuration."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LIB = ROOT.parents[1] / 'libraries/HAS1BleBeacon/src'
blackbox = (ROOT / 'blackbox.ino').read_text()
begin = blackbox[blackbox.index('void BlackBoxBegin() {'):blackbox.index('void BlackBoxLoop() {')]
source = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "generator_ble.h"
#define HAS1_GENERATOR_BLACKBOX_ENABLED 1
#define HAS1_GENERATOR_BLACKBOX_BROKER_URI "mqtt://test"
#define HAS1_GENERATOR_BLACKBOX_MQTT_USERNAME "test-user"
#define HAS1_GENERATOR_BLACKBOX_MQTT_PASSWORD "test-password"
#define FIRMWARE_VER 40
#define PARTITION_VER 1
static unsigned begins=0, polls=0, names=0, stops=0, releases=0;
#if HAS1_GENERATOR_BLE_ENABLED
namespace Has1BleBeacon {
void begin(){++begins;}
void poll(bool allowed){assert(allowed);++polls;}
void setDeviceName(const char* name){assert(!strcmp(name,"SG"));++names;}
void suspend(){++stops;}
bool releaseMemory(){++releases;return true;}
Diagnostics diagnostics(){return {Step::Running,RadioState::On,Error::None,0,42,false,false,7,0};}
}
#endif
using WiFiEvent_t=int;
using WiFiEventInfo_t=int;
static unsigned subscriptions=0;
struct Wifi {template<class F>void onEvent(F){++subscriptions;}} WiFi;
int esp_reset_reason(){return 1;}
namespace has1blackbox {
enum class EventType {Boot,State};
struct Config {const char *device,*version,*broker;bool enabled;const char *user,*password;};
struct Logger {
  unsigned starts=0;std::vector<const char*> events;
  void begin(Config c){++starts;assert(c.enabled);assert(!strcmp(c.broker,"mqtt://test"));
    assert(!strcmp(c.device,"HAS1_generator"));assert(!strcmp(c.version,"40"));
    assert(!strcmp(c.user,"test-user") && !strcmp(c.password,"test-password"));}
  void log(EventType,int a,int b,int,int,const char* message){
    events.push_back(message);
    if(!strcmp(message,"build.ble.blackbox")){assert(a==HAS1_GENERATOR_BLE_ENABLED && b==1);}
  }
} blackbox;
void logWifiMetadata(int,int){}
}
'''
tests = r'''
int main(){
  BlackBoxBegin();
  assert(has1blackbox::blackbox.starts==1 && subscriptions==1);
  assert(has1blackbox::blackbox.events.size()==2);
  assert(!strcmp(has1blackbox::blackbox.events[0],"device.boot"));
  assert(!strcmp(has1blackbox::blackbox.events[1],"build.ble.blackbox"));
  GeneratorBle::setDeviceName("SG");GeneratorBle::begin();
  for(unsigned i=0;i<1000;++i)GeneratorBle::poll();
  GeneratorBle::suspend();
  assert(GeneratorBle::releaseMemory()==GeneratorBle::enabled());
  auto d=GeneratorBle::diagnostics();
  if(GeneratorBle::enabled()){
    assert(begins==1 && polls==1000 && names==1 && stops==1 && releases==1);
    assert(d.pendingOpcode==42 && d.sentCommands==7);
    assert(!strcmp(GeneratorBle::mode(),"enabled"));
  }else{
    assert(begins==0 && polls==0 && names==0 && stops==0 && releases==0);
    assert(d.pendingOpcode==0 && d.sentCommands==0 && !d.awaitingCompletion);
    assert(!strcmp(GeneratorBle::mode(),"disabled"));
    assert(strstr(GeneratorBle::bootMessage(),"[BLE] disabled"));
  }
  puts("PASS: BLE mode, OTA no-op/delegation and preserved blackbox boot/configuration");
}
'''
with tempfile.TemporaryDirectory(prefix='generator-ble-') as directory:
    directory = Path(directory)
    cpp = directory / 'config.cpp'
    cpp.write_text(source + begin + tests)
    poison = directory / 'poison'
    poison.mkdir()
    (poison / 'HAS1BleBeacon.h').write_text('#error "Disabled build imported Bluetooth library"\n')
    for enabled in (None, 0, 1):
        binary = directory / f'config-{enabled}'
        flags = [] if enabled is None else [f'-DHAS1_GENERATOR_BLE_ENABLED={enabled}']
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra',
                        '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I', str(ROOT), '-I', str(LIB if enabled == 1 else poison),
                        *flags, str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
