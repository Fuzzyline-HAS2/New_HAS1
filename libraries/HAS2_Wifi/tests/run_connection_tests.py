#!/usr/bin/env python3
"""Exercise the production cooperative Wi-Fi controller with a fake driver."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include "has2_wifi_connection.h"
#include <cassert>
#include <cstdio>
using C=Has2Connection::Controller;
struct Driver {
 bool starts=true, started=true, online=false, connecting=true;
 int scan=-1, available=1, selected=0;
 int startsCount=0, scans=0, connects=0, disconnects=0, cancels=0, released=0, connectedCalls=0;
 bool startStation(){++startsCount;return starts;}
 bool stationStarted(){return started;}
 bool connected(){return online;}
 void disconnect(){++disconnects;}
 int startScan(){++scans;return -1;}
 int scanResult(){return scan;}
 void cancelScan(){++cancels;}
 void releaseScan(){++released;}
 bool selectCandidates(int){selected=0;return available>0;}
 bool connectCandidate(){++connects;return connecting;}
 bool advanceCandidate(){return ++selected<available;}
 void onConnected(){++connectedCalls;}
};
void scanning(C&c,Driver&d,uint32_t t=0) {
 c.begin(t);assert(d.scans==0 && d.connects==0 && d.startsCount==0);
 assert(!c.service(t,d)); assert(!c.service(t,d));
 assert(c.state()==C::State::Settle);
 assert(!c.service(t+49,d));assert(d.scans==0);
 assert(!c.service(t+50,d));assert(d.scans==1 && c.state()==C::State::Scan);
}
void connecting(C&c,Driver&d,uint32_t t=0) {
 scanning(c,d,t);d.scan=1;c.service(t+51,d);c.service(t+101,d);
 assert(c.state()==C::State::Connect && d.connects==1);
}
int main(){
 assert(!Has2Connection::scanCancellationExpired(3999,0));
 assert(Has2Connection::scanCancellationExpired(4000,0));
 assert(!Has2Connection::scanCancellationExpired(3898,UINT32_MAX-100));
 assert(Has2Connection::scanCancellationExpired(3899,UINT32_MAX-100));
 { C c;Driver d;assert(!c.service(100,d));assert(d.startsCount==0); }
 { C c;Driver d;connecting(c,d);d.online=true;assert(c.service(102,d));
   assert(c.state()==C::State::Online && d.connectedCalls==1);
   for(int i=0;i<100;++i)assert(c.service(200+i,d));
   assert(d.scans==1 && d.connects==1 && d.connectedCalls==1); }
 { C c;Driver d;scanning(c,d);for(int t=51;t<6050;t+=25)c.service(t,d);
   c.service(6050,d);assert(c.state()==C::State::Backoff && d.cancels==1);
   c.service(8049,d);assert(d.scans==1);c.service(8050,d);
   assert(c.state()==C::State::Start); }
 { C c;Driver d;d.started=false;c.begin(0);c.service(0,d);c.service(999,d);
   assert(c.state()==C::State::Station);c.service(1000,d);
   assert(c.state()==C::State::Backoff && d.scans==0); }
 { C c;Driver d;d.starts=false;c.begin(0);c.service(0,d);
   assert(c.retryDelay()==2000);c.service(2000,d);c.service(2000,d);
   assert(c.retryDelay()==4000);c.service(6000,d);c.service(6000,d);
   assert(c.retryDelay()==8000);c.service(14000,d);c.service(14000,d);
   assert(c.retryDelay()==8000); }
 { C c;Driver d;d.available=3;connecting(c,d);
   c.service(3100,d);assert(d.connects==1);c.service(3101,d);
   assert(c.state()==C::State::Settle);c.service(3151,d);assert(d.connects==2);
   c.service(6151,d);c.service(6201,d);assert(d.connects==3);
   c.service(9201,d);assert(c.state()==C::State::Backoff && d.scans==1); }
 { C c;Driver d;scanning(c,d);d.scan=0;d.available=0;c.service(51,d);
   assert(c.state()==C::State::Backoff && d.connects==0 && d.released==1); }
 { C c;Driver d;scanning(c,d);d.scan=-2;c.service(51,d);
   assert(c.state()==C::State::Backoff && d.cancels==1); }
 { C c;Driver d;connecting(c,d);d.online=true;c.service(102,d);d.online=false;
   c.service(103,d);assert(c.state()==C::State::Settle);c.service(153,d);
   assert(d.scans==2);d.scan=1;c.service(154,d);c.service(204,d);
   d.online=true;assert(c.service(205,d));assert(d.connectedCalls==2); }
 { C c;Driver d;d.online=true;c.begin(0);c.service(0,d);c.service(0,d);
   c.service(50,d);assert(c.state()==C::State::Settle && d.scans==0);
   c.service(1000,d);assert(c.state()==C::State::Backoff && d.connectedCalls==0); }
 { C c;Driver d;connecting(c,d,UINT32_MAX-100);
   c.service(2999,d);assert(c.state()==C::State::Connect);
   c.service(3000,d);assert(c.state()==C::State::Backoff); }
 { C c;Driver d;d.starts=false;c.begin(0);c.service(0,d);c.service(2000,d);
   d.starts=true;c.service(2000,d);c.service(2000,d);c.service(2050,d);
   d.scan=1;c.service(2051,d);c.service(2101,d);d.online=true;c.service(2102,d);
   d.online=false;c.service(2103,d);c.service(2153,d);d.scan=-2;c.service(2154,d);
   assert(c.retryDelay()==2000); }
 { C c;Driver d;d.connecting=false;d.available=2;scanning(c,d);d.scan=1;
   c.service(51,d);c.service(101,d);assert(c.state()==C::State::Settle);
   c.service(151,d);assert(c.state()==C::State::Backoff && d.connects==2); }
 std::puts("Wi-Fi connection controller: 13 cases passed");
}
'''
with tempfile.TemporaryDirectory(prefix="wifi-controller-") as tmp:
    src=Path(tmp)/"test.cpp";exe=Path(tmp)/"test"
    src.write_text(SOURCE)
    subprocess.run(["c++","-std=c++17","-Wall","-Wextra","-Werror","-I",str(ROOT),str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
# Ensure the adapter cannot silently revert to the old blocking/reboot path.
cpp=(ROOT/"HAS2_Wifi.cpp").read_text()
adapter=cpp.split("struct HAS2_Wifi::ConnectionBackend {",1)[1].split("void HAS2_Wifi::Setup()",1)[0]
for forbidden in ("delay(","ESP.restart(","TryConnect(","TryConnectOrdered(","ReceiveMine(","http.GET(","WiFi.scanNetworks()"):
    assert forbidden not in adapter, forbidden
assert "WiFi.scanNetworks(true)" in adapter
assert "WiFi.disconnectAsync(false, false)" in adapter
print("Wi-Fi adapter blocking-path guards passed")
# Compile the actual scan adapter methods: cancellation must not free a result
# while the Arduino event task can still be allocating/copying it.
scan_methods=cpp.split('  int startScan() {',1)[1].split('  bool selectCandidates',1)[0]
scan_source=r'''
#include <stdint.h>
#include <cassert>
#include <cstdio>
constexpr int WIFI_SCAN_RUNNING=-1,WIFI_SCAN_FAILED=-2,WIFI_SCAN_DONE_BIT=2,WIFI_SCANNING_BIT=1;
static int flags=0,stops=0;
static unsigned millis(){return 100;}
struct WiFiGenericClass {static int getStatusBits(){return flags;}};
struct MockWiFi {
 int scans=0,reads=0,frees=0;
 int scanNetworks(bool async){assert(async);++scans;return WIFI_SCAN_RUNNING;}
 int scanComplete(){assert(flags&WIFI_SCAN_DONE_BIT);++reads;return 3;}
 void scanDelete(){assert(flags&WIFI_SCAN_DONE_BIT);++frees;flags=0;}
} WiFi;
void esp_wifi_scan_stop(){++stops;}
struct Owner {bool connectionScanPending=false,connectionScanCancelled=false;unsigned connectionCancelStarted=0;};
struct Adapter {Owner&owner;
  int startScan() {
'''+scan_methods+r'''
};
int main(){
 Owner o;Adapter a{o};assert(a.startScan()==WIFI_SCAN_RUNNING);
 assert(a.scanResult()==WIFI_SCAN_RUNNING);a.cancelScan();a.cancelScan();a.releaseScan();
 assert(stops==1&&WiFi.frees==0&&WiFi.reads==0);
 for(int i=0;i<10;++i){assert(a.startScan()==WIFI_SCAN_RUNNING);assert(a.scanResult()==WIFI_SCAN_RUNNING);}
 assert(WiFi.scans==1);flags=WIFI_SCAN_DONE_BIT|WIFI_SCANNING_BIT;
 assert(a.scanResult()==WIFI_SCAN_RUNNING);a.releaseScan();assert(WiFi.frees==0);
 flags=WIFI_SCAN_DONE_BIT;
 assert(a.scanResult()==WIFI_SCAN_FAILED);assert(WiFi.frees==1&&!o.connectionScanPending);
 a.cancelScan();assert(stops==1);
 assert(a.startScan()==WIFI_SCAN_RUNNING);flags=WIFI_SCAN_DONE_BIT;
 assert(a.scanResult()==3);assert(WiFi.reads==1);a.releaseScan();
 assert(WiFi.frees==2&&!o.connectionScanPending);a.releaseScan();assert(WiFi.frees==2);
 puts("Wi-Fi scan cancellation/result barrier tests passed");
}
'''
with tempfile.TemporaryDirectory(prefix="wifi-scan-barrier-") as tmp:
    src=Path(tmp)/"test.cpp";exe=Path(tmp)/"test"
    src.write_text(scan_source)
    subprocess.run(["c++","-std=c++17","-Wall","-Wextra","-Werror",str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
service=cpp.split('bool HAS2_Wifi::ServiceConnection() {',1)[1].split('\nbool HAS2_Wifi::ApplyNetworkSnapshot',1)[0]
fault_source=r'''
#include "has2_wifi_connection.h"
#include <cassert>
#include <string>
#include <cstdio>
static uint32_t clockNow=0;
uint32_t millis(){return clockNow;}
struct HAS2_Wifi {
 std::string my_mac="valid";
 bool connectionDriverFault=false,connectionScanPending=false,connectionScanCancelled=false,done=false;
 uint32_t connectionCancelStarted=0;
 struct ConnectionBackend {HAS2_Wifi&owner;bool scanFinished(){return owner.done;}};
 struct Control {int calls=0;bool service(uint32_t,ConnectionBackend&){++calls;return false;}} connectionController;
 bool ServiceConnection();
};
bool HAS2_Wifi::ServiceConnection() {
'''+service+r'''
int main(){
 HAS2_Wifi w;w.connectionScanPending=w.connectionScanCancelled=true;
 clockNow=3999;w.ServiceConnection();assert(!w.connectionDriverFault&&w.connectionController.calls==1);
 clockNow=4000;w.ServiceConnection();assert(w.connectionDriverFault&&w.connectionController.calls==1);
 w.done=true;clockNow=5000;w.ServiceConnection();assert(w.connectionDriverFault&&w.connectionController.calls==1);
 HAS2_Wifi late;late.connectionScanPending=late.connectionScanCancelled=late.done=true;
 late.ServiceConnection();assert(!late.connectionDriverFault&&late.connectionController.calls==1);
 HAS2_Wifi offline;clockNow=1000000;offline.ServiceConnection();assert(!offline.connectionDriverFault&&offline.connectionController.calls==1);
 puts("Wi-Fi missing completion fault/normal offline tests passed");
}
'''
with tempfile.TemporaryDirectory(prefix="wifi-scan-fault-") as tmp:
    src=Path(tmp)/"test.cpp";exe=Path(tmp)/"test"
    src.write_text(fault_source)
    subprocess.run(["c++","-std=c++17","-Wall","-Wextra","-Werror","-I",str(ROOT),str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
# Compile actual callback/getter against a host mutex: every copied snapshot
# must describe one complete event even while the event task keeps publishing.
diag=cpp.split('static portMUX_TYPE connectionDisconnectMux',1)[1].split('void HAS2_Wifi::BeginConnection',1)[0]
diag=diag.replace('static wifi_event_id_t connectionDisconnectEventId = 0;','')
diag_source=r'''
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <thread>
#include <atomic>
using portMUX_TYPE=std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(m) (m)->lock()
#define portEXIT_CRITICAL(m) (m)->unlock()
using arduino_event_id_t=int;
constexpr int ARDUINO_EVENT_WIFI_STA_DISCONNECTED=5;
struct arduino_event_info_t { struct { uint8_t reason; } wifi_sta_disconnected; };
struct HAS2_WifiDisconnectDiagnostic {uint32_t sequence=0,atMs=0;uint8_t reason=0;};
struct HAS2_Wifi {HAS2_WifiDisconnectDiagnostic GetDisconnectDiagnostic() const;};
static uint32_t now=0;
uint32_t millis(){return now;}
static portMUX_TYPE connectionDisconnectMux
'''+diag+r'''
int main(){
 HAS2_Wifi wifi;assert(wifi.GetDisconnectDiagnostic().sequence==0);
 ConnectionDisconnectEvent(0,{});assert(wifi.GetDisconnectDiagnostic().sequence==0);
 std::atomic<bool> done{false};
 std::thread events([&]{for(uint32_t i=1;i<=10000;++i){now=i;ConnectionDisconnectEvent(5,{{uint8_t(i%255)}});}done.store(true);});
 do {auto d=wifi.GetDisconnectDiagnostic();assert(d.sequence==d.atMs);assert(d.reason==d.sequence%255);}while(!done.load());
 events.join();assert(wifi.GetDisconnectDiagnostic().sequence==10000);
 connectionDisconnectDiagnostic.sequence=UINT32_MAX;now=123;ConnectionDisconnectEvent(5,{{201}});
 auto d=wifi.GetDisconnectDiagnostic();assert(d.sequence==1&&d.atMs==123&&d.reason==201);
 puts("Wi-Fi disconnect callback/snapshot concurrency tests passed");
}
'''
with tempfile.TemporaryDirectory(prefix="wifi-disconnect-") as tmp:
    src=Path(tmp)/"test.cpp";exe=Path(tmp)/"test"
    src.write_text(diag_source)
    subprocess.run(["c++","-std=c++17","-Wall","-Wextra","-Werror","-pthread","-fsanitize=address,undefined",str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
