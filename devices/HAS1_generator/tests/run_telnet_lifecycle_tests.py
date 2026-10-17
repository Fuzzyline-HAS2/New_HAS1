#!/usr/bin/env python3
"""Actual Telnet session lifecycle with deterministic sockets/time; no hardware."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'telnet.ino').read_text()
STALL_POLICY = next(line for line in SOURCE.splitlines()
                    if line.startswith('static constexpr uint32_t kTelnetSendStallMs'))


def function(name):
    start = SOURCE.index(name)
    opening = SOURCE.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[start:end]


PREAMBLE = r'''
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <memory>
#include <algorithm>
#include "telnet_log_buffer.h"
#define MSG_DONTWAIT 64
#define portENTER_CRITICAL(x) do { (void)(x); } while(0)
#define portEXIT_CRITICAL(x) do { (void)(x); } while(0)
static int telnetLogMux;
static TelnetLogBuffer<8192> telnetHistory;
static bool telnetSessionActive=false;
static uint64_t telnetPosition=0,telnetReplayEnd=0;
static bool telnetReplayEnded=false;
static char telnetControl[1024],telnetRetainedCrash[200];
static size_t telnetControlLength=0,telnetControlSent=0;
static unsigned telnetPriorLine=0;
static bool telnetPriorDone=true;
@@STALL_POLICY@@
static bool telnetSendPending=false;
static uint32_t telnetLastSendProgress=0;
static uint32_t now=0;
static uint32_t millis(){return now;}
static void TelnetDrain();
struct Socket {int fd;bool connected=true;bool stopped=false;bool endless=false;bool noDelay=false;bool readError=false;unsigned reads=0,stops=0;Socket(int f):fd(f){} };
struct WiFiClient {
 std::shared_ptr<Socket> p;
 WiFiClient(){} explicit WiFiClient(int fd):p(std::make_shared<Socket>(fd)){}
 int fd(){return p?p->fd:-1;}
 bool connected(){return p&&p->connected;}
 void stop(){if(p){p->stopped=true;p->connected=false;p->fd=-1;++p->stops;}}
 void setNoDelay(bool v){if(p)p->noDelay=v;}
 int available(){return p&&p->connected&&p->endless?1000000:0;}
 int read(){assert(available());++p->reads;return p->readError?-1:'X';}
} telnetClient;
struct Server {
 std::deque<WiFiClient> pending;
 bool hasClient(){return !pending.empty();}
 WiFiClient available(){auto c=pending.front();pending.pop_front();return c;}
} telnetServer;
static std::map<int,std::string> output;
static std::vector<int> plan;
static size_t planIndex=0,calls=0;
static int defaultSend=256;
static int send(int fd,const void *data,size_t len,int flags){
 assert(fd>=0&&flags==MSG_DONTWAIT&&len<=256);++calls;
 int n=planIndex<plan.size()?plan[planIndex++]:defaultSend;
 if(n<0){errno=-n;return -1;}n=std::min(n,int(len));output[fd].append(static_cast<const char*>(data),n);return n;
}
static std::vector<std::string> prior={"[GD prior] header\r\n","[GD prior] 0\r\n","[GD prior] 1\r\n"};
static size_t DiagnosticPriorLine(unsigned i,char *buf,size_t cap){
 if(i>=prior.size())return 0;assert(prior[i].size()<cap);memcpy(buf,prior[i].data(),prior[i].size());return prior[i].size();
}
static size_t TelnetFormatDiagnostics(char *buf,size_t cap){
 return snprintf(buf,cap,"[diagnostics] host mock\r\n");
}
struct Debug {
 void println(const char *s){telnetHistory.append(reinterpret_cast<const uint8_t*>(s),strlen(s));static const uint8_t nl[]={'\r','\n'};telnetHistory.append(nl,2);}
} DebugSerial;
static void append(const std::string&s){telnetHistory.append(reinterpret_cast<const uint8_t*>(s.data()),s.size());}
static void reset(){telnetHistory=TelnetLogBuffer<8192>();telnetClient=WiFiClient();telnetServer.pending.clear();telnetSessionActive=false;telnetPosition=telnetReplayEnd=0;telnetReplayEnded=false;telnetControlLength=telnetControlSent=0;telnetPriorLine=0;telnetPriorDone=true;telnetRetainedCrash[0]=0;output.clear();plan.clear();planIndex=calls=0;defaultSend=256;now=0;telnetSendPending=false;telnetLastSendProgress=0;prior={"[GD prior] header\r\n","[GD prior] 0\r\n","[GD prior] 1\r\n"};}
'''

TESTS = r'''
static void run(unsigned loops=30){for(unsigned i=0;i<loops;++i){auto before=calls;TelnetRun();assert(calls-before<=5);}}
static void tick(uint32_t delta){now+=delta;run(1);}
static unsigned count(const std::string&s,const std::string&needle){unsigned n=0;size_t p=0;while((p=s.find(needle,p))!=std::string::npos){++n;p+=needle.size();}return n;}
static WiFiClient connect(int fd=7){WiFiClient c(fd);telnetServer.pending.push_back(c);run();return c;}
int main(){
 assert(kTelnetSendStallMs==5000);
 reset();append("boot\r\n");strcpy(telnetRetainedCrash,"reset=TASK_WDT");
 for(int fd=7;fd<207;++fd){
  WiFiClient c=connect(fd);
  assert(telnetClient.p==c.p&&c.p->noDelay&&telnetSessionActive);
  const auto&o=output[fd];assert(count(o,"[diagnostics]")==1);assert(count(o,"reset=TASK_WDT")==1);
  for(const auto&l:prior)assert(count(o,l)==1);
  assert(count(o,"replay end / live output")==1);
  assert(telnetPriorDone&&telnetPriorLine==prior.size()+1);
  c.p->connected=false;run(1);assert(!telnetSessionActive&&c.p->stopped);
 }
 puts("PASS 200 reconnects restart prior replay and clear session state");

 reset();WiFiClient first=connect(),second(8);auto historyPosition=telnetPosition;
 telnetServer.pending.push_back(second);run(1);
 assert(second.p->stopped&&telnetClient.p==first.p&&telnetSessionActive&&!first.p->stopped);
 assert(output[8]=="Telnet already connected.\r\n"&&telnetPosition==historyPosition);
 puts("PASS active healthy collector retained when another collector connects");

 first.p->endless=true;
 for(unsigned i=0;i<1000;++i){auto reads=first.p->reads;auto before=calls;tick(100);assert(first.p->reads-reads==64&&calls-before<=4);}
 assert(!first.p->stopped&&telnetSessionActive);
 puts("PASS idle collector is not expired; incoming flood bounded to 64 reads per loop");

 reset();defaultSend=-EAGAIN;WiFiClient blocked=connect();blocked.p->endless=true;
 assert(telnetSendPending&&telnetSessionActive&&output[7].empty());
 for(unsigned i=0;i<49;++i){auto reads=blocked.p->reads;tick(100);assert(blocked.p->reads-reads==64&&!blocked.p->stopped);}
 WiFiClient replacement(8);telnetServer.pending.push_back(replacement);now=5000;run(1);
 assert(blocked.p->stopped&&telnetSessionActive&&telnetClient.p==replacement.p&&!replacement.p->stopped);
 assert(output[8].find("Telnet already connected.")==std::string::npos);
 defaultSend=256;run();for(const auto&l:prior)assert(count(output[8],l)==1);
 puts("PASS 5s EAGAIN timeout releases stale collector before accepting queued replacement; input cannot extend deadline");

 reset();defaultSend=0;WiFiClient zero=connect();tick(4999);assert(!zero.p->stopped);
 tick(1);assert(zero.p->stopped&&!telnetSessionActive);
 reset();defaultSend=-EINTR;WiFiClient interrupted=connect();tick(5000);assert(interrupted.p->stopped&&!telnetSessionActive);
 puts("PASS zero-byte and interrupted sends cannot retain a pending session forever");

 reset();defaultSend=-EAGAIN;WiFiClient progress=connect();tick(4000);
 plan={1,-EAGAIN};planIndex=0;run(1);assert(output[7].size()==1&&!progress.p->stopped);
 tick(4999);assert(!progress.p->stopped);tick(1);assert(progress.p->stopped);
 puts("PASS actual partial send progress restarts no-progress deadline");

 reset();WiFiClient idle=connect();tick(60000);assert(!idle.p->stopped&&!telnetSendPending);
 append("fresh after idle\n");defaultSend=-EAGAIN;run(1);assert(!idle.p->stopped&&telnetSendPending);
 tick(4999);assert(!idle.p->stopped);tick(1);assert(idle.p->stopped);
 puts("PASS new output after a long idle starts a fresh timeout");

 reset();defaultSend=1;WiFiClient gap(7);telnetServer.pending.push_back(gap);run(1);
 assert(telnetControlSent==4&&!telnetSendPending);tick(60000);
 assert(!gap.p->stopped&&telnetControlSent==8);
 puts("PASS main-loop gap after successful sends does not falsely expire collector");

 reset();now=UINT32_MAX-2499;defaultSend=-EAGAIN;WiFiClient wrapped=connect();
 tick(4999);assert(!wrapped.p->stopped);tick(1);assert(wrapped.p->stopped);
 puts("PASS no-progress deadline spans millis rollover");

 reset();defaultSend=-EAGAIN;WiFiClient recovered=connect();
 defaultSend=1;WiFiClient extra(8);telnetServer.pending.push_back(extra);
 auto beforeProbe=calls;tick(60000);
 assert(!recovered.p->stopped&&telnetClient.p==recovered.p&&extra.p->stopped);
 assert(output[7].size()==4&&calls-beforeProbe==5&&!telnetSendPending);
 puts("PASS expired candidate gets one bounded send probe; resumed reader survives main-loop gap without double drain");

 reset();prior.clear();std::string expectedPrior;
 for(unsigned i=0;i<33;++i){prior.push_back("[GD prior] "+std::to_string(i)+" "+std::string(100,'A'+i%26)+"\r\n");expectedPrior+=prior.back();}
 append("historic payload\n");defaultSend=3;WiFiClient slow(7);telnetServer.pending.push_back(slow);
 for(unsigned i=0;i<2000;++i){tick(100);assert(!slow.p->stopped);if(telnetPriorDone&&telnetReplayEnded&&telnetPosition==telnetHistory.end())break;}
 assert(telnetPriorDone&&telnetReplayEnded&&telnetPosition==telnetHistory.end());
 assert(output[7].find(expectedPrior)!=std::string::npos);
 assert(count(output[7],"historic payload\n")==1&&count(output[7],"replay end / live output")==1);
 assert(now>5000);
 puts("PASS replay larger than control buffer completes over 5s through continual tiny partial sends");

 reset();WiFiClient failed(7);telnetServer.pending.push_back(failed);defaultSend=-ECONNRESET;run(1);
 assert(failed.p->stopped&&!telnetSessionActive);auto stopped=failed.p->stops;run(1);assert(failed.p->stops==stopped);
 puts("PASS fatal send immediately releases socket and active session");

 reset();WiFiClient invalid(-1);telnetServer.pending.push_back(invalid);run(1);
 assert(invalid.p->stopped&&!telnetSessionActive&&calls==0);
 puts("PASS invalid descriptor releases session without send attempts");

 reset();WiFiClient old=connect();old.p->connected=false;WiFiClient fresh(8);telnetServer.pending.push_back(fresh);run();
 assert(old.p->stopped&&telnetSessionActive&&telnetClient.p==fresh.p);
 for(const auto&l:prior)assert(count(output[8],l)==1);
 puts("PASS disconnected socket replaced in same loop with fresh replay");

 reset();WiFiClient readFailed=connect();readFailed.p->endless=true;readFailed.p->readError=true;
 auto before=readFailed.p->reads;run(1);assert(readFailed.p->reads-before<=64);
 puts("PASS incoming read failure remains bounded");
 return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='generator-telnet-lifecycle-') as tmp:
    source = Path(tmp) / 'test.cpp'
    source.write_text(PREAMBLE.replace('@@STALL_POLICY@@', STALL_POLICY) + '\n' + '\n'.join(function(name) for name in (
        'static void TelnetCloseSession(', 'static bool TelnetHasPendingOutput(',
        'static bool TelnetExpireStalledSession(', 'static void TelnetStartSession(',
        'static void TelnetDrain() {', 'void TelnetRun(')) + TESTS)
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(ROOT), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
