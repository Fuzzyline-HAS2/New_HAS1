#!/usr/bin/env python3
"""Run production RTC journal and logger with fake time/reset and a memory sink."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
PREAMBLE=r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "generator_diagnostics.h"
#define RTC_NOINIT_ATTR
#define FIRMWARE_VER 26
static unsigned criticalDepth=0;
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) do { (void)x; assert(++criticalDepth==1); } while(0)
#define portEXIT_CRITICAL(x) do { (void)x; assert(--criticalDepth==0); } while(0)
static uint32_t clockMs=0;
unsigned long millis() { return clockMs; }
enum esp_reset_reason_t { ESP_RST_UNKNOWN,ESP_RST_POWERON,ESP_RST_EXT,ESP_RST_SW,
 ESP_RST_PANIC,ESP_RST_INT_WDT,ESP_RST_TASK_WDT,ESP_RST_WDT,ESP_RST_DEEPSLEEP,ESP_RST_BROWNOUT };
static esp_reset_reason_t resetReason=ESP_RST_POWERON;
esp_reset_reason_t esp_reset_reason() { return resetReason; }
struct FakeEsp { uint32_t getFreeHeap(){return 100000;} uint32_t getMinFreeHeap(){return 90000;}
 uint32_t getMaxAllocHeap(){return 80000;} } ESP;
struct FakeWifi { int status(){return 3;} int RSSI(){return -60;} } WiFi;
struct FakeShift { int watchdog=0; int operator[](const char *)const{return watchdog;} } shift_machine;
static std::string output;
static size_t largestWrite=0;
void TelnetDiagnosticWrite(const char *text,size_t size) {
 assert(criticalDepth==0 && size<=319); largestWrite=std::max(largestWrite,size); output.append(text,size);
}
'''
TESTS=r'''
using namespace GeneratorDiagnostic;
static Record record(uint32_t seq) {
 Record r={}; r.sequence=seq;r.at=seq*10;r.id=seq;r.detail=99;r.event=Neo;r.phase=Begin;return r;
}
static void journalTests() {
 volatile Journal j={}; Record prior[Capacity]; unsigned invalid=0;
 initialize(j,26); assert(recover(j,26,true,prior,invalid)==0);
 for(unsigned n=1;n<=100;++n) append(j,record(n));
 assert(recover(j,26,true,prior,invalid)==32 && prior[0].sequence==69 && prior[31].sequence==100);
 assert(recover(j,26,false,prior,invalid)==0 && recover(j,27,true,prior,invalid)==0);
 j.schema=99; assert(recover(j,26,true,prior,invalid)==0); j.schema=Schema;
 j.checksum^=1; assert(recover(j,26,true,prior,invalid)==0); j.checksum=headerChecksum(26);
 puts("PASS 32-record chronological wrap; cold/firmware/schema/header corruption rejected");
 j.records[99%Capacity].detail^=1;
 assert(recover(j,26,true,prior,invalid)==31 && invalid==1);
 j.records[98%Capacity].checksum=0; j.records[98%Capacity].sequence=200; // Reset during commit.
 assert(recover(j,26,true,prior,invalid)==30);
 puts("PASS corrupt payload and torn writes ignored without trusting indexes");
 initialize(j,26);
 for(uint32_t seq=UINT32_MAX-10;seq!=0;++seq) append(j,record(seq));
 for(uint32_t seq=1;seq<=10;++seq) append(j,record(seq));
 const unsigned count=recover(j,26,true,prior,invalid);
 assert(count==21 && prior[0].sequence==UINT32_MAX-10 && prior[count-1].sequence==10);
 puts("PASS sequence wrap and nonzero sequence ordering");
 RateGate gate; assert(gate.allow(UINT32_MAX-50,500));
 assert(!gate.allow(100,500)); assert(gate.allow(500,500)); assert(gate.skipped==1);
 puts("PASS rate limits across millis rollover; denied events do not push deadline");
}
static void runtimeTests() {
 resetReason=ESP_RST_POWERON; DiagnosticInit(); output.clear();
 DiagnosticHttpHook("Loop","request.begin",0,0,0);
 DiagnosticHttpHook("Loop","get.end",200,700,0);
 DiagnosticHttpHook("Loop","body.end",0,800,10);
 DiagnosticHttpHook("Loop","close.begin",200,800,10);
 // Simulate a reset while close is pending: preserve records, reset ordinary globals.
 resetReason=ESP_RST_TASK_WDT; diagnosticSequence=diagnosticId=0;
 DiagnosticInit(); assert(diagnosticPriorCount>=6);
 std::string previous;
 char line[320];
 for(unsigned n=0;;++n){size_t len=DiagnosticPriorLine(n,line,sizeof(line));if(!len)break;previous.append(line,len);}
 assert(previous.find("stage=close.begin kind=Loop")!=std::string::npos);
 assert(previous.find("has2_http BEGIN")!=std::string::npos);
 for(unsigned n=0;n<200;++n) DiagnosticPoint(Wire,n,n);
 std::string again;
 for(unsigned n=0;;++n){size_t len=DiagnosticPriorLine(n,line,sizeof(line));if(!len)break;again.append(line,len);}
 assert(previous==again);
 puts("PASS reset during close retains stage/kind; previous snapshot survives current journal overwrite and reconnect");
 resetReason=ESP_RST_BROWNOUT; DiagnosticInit(); assert(diagnosticPriorCount==0);
 resetReason=ESP_RST_POWERON; DiagnosticInit(); assert(diagnosticPriorCount==0);
 puts("PASS brownout/power-on always discard untrustworthy RTC data");
 output.clear(); diagnosticNormal[Neo]=RateGate(); diagnosticErrors[Neo]=RateGate();
 clockMs=1000;
 for(unsigned i=0;i<1000;++i){DiagnosticScope scope(Neo,2);scope.result(-1,2);}
 size_t lines=0;for(char c:output)if(c=='\n')++lines;
 assert(lines==2); clockMs=2000;{DiagnosticScope scope(Neo,2);scope.result(-1,2);}
 assert(output.find("suppressed=999")!=std::string::npos);
 puts("PASS 1000 repeated faults bounded to one BEGIN/END pair per second and suppression summary");
 output.clear(); diagnosticNormal[Has2Http]=RateGate(); diagnosticErrors[Has2Http]=RateGate();
 DiagnosticHttpHook("Receive","request.begin",0,0,0);
 DiagnosticHttpHook("Receive","request.end",1,1,10);
 DiagnosticHttpHook("Receive","request.begin",0,0,0);
 DiagnosticHttpHook("Receive","request.end",0,1,0);
 assert(output.find("END code=-1")!=std::string::npos);
 puts("PASS fast HTTP failures remain visible even after normal trace throttling");
 shift_machine.watchdog=1;
 DiagnosticHttpHook("Loop","request.begin",0,0,0);
 DiagnosticHttpHook("Loop","request.end",1,1,10);
 assert(output.find("server_watchdog=1 observed")!=std::string::npos);
 Record records[Capacity];unsigned invalid=0;
 const unsigned n=recover(diagnosticRtc,26,true,records,invalid);
 assert(n && records[n-1].event==ServerReset && records[n-1].detail==1);
 shift_machine.watchdog=0;
 puts("PASS successful server watchdog flag retained separately from hardware watchdog reset reason");
 output.clear(); DiagnosticPrintf("%s",std::string(5000,'A').c_str());
 assert(output.size()==319 && output.substr(317)=="\r\n" && largestWrite<=319);
 puts("PASS formatter length bounded and truncation remains line-delimited; sink outside every lock");
}
int main(){journalTests();runtimeTests();return 0;}
'''
source=(ROOT/'diagnostics.ino').read_text()
# Any UART/heap/network dependency introduced into the diagnostic core must be reviewed.
for token in ('Serial.', 'DebugSerial.', 'malloc(', 'new ', 'send(', '.write(', 'nvs_'):
    assert token not in source, f'unexpected blocking/allocation path: {token}'
with tempfile.TemporaryDirectory(prefix='generator-diagnostic-test-') as folder:
    d=Path(folder)
    (d/'esp_system.h').write_text('// reset reason stubs in test preamble\n')
    (d/'esp_attr.h').write_text('// RTC attribute stub in test preamble\n')
    (d/'test.cpp').write_text(PREAMBLE+source+TESTS)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++11','-Wall','-Wextra','-Werror',
      '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(d),'-I',str(ROOT),
      str(d/'test.cpp'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True)
