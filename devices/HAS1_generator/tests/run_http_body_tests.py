#!/usr/bin/env python3
"""Compile the production bounded reader, HTTP request and polling integration."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
LIB = ROOT / 'libraries/HAS2_Wifi'
include = Path(os.environ.get('ARDUINOJSON_INCLUDE', str(Path.home() / 'Documents/Arduino/libraries/ArduinoJson/src')))
if not (include / 'ArduinoJson.h').is_file():
    raise SystemExit('Set ARDUINOJSON_INCLUDE to ArduinoJson src directory')

def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

cpp = (LIB / 'HAS2_Wifi.cpp').read_text()
header = (LIB / 'HAS2_Wifi.h').read_text()
functions = '\n'.join(function(cpp, signature) for signature in [
    'void HAS2_Wifi::EnableBoundedResponseBody()', 'void HAS2_Wifi::SetHttpDiagnosticHook(', 'void HAS2_Wifi::Receive(String',
    'void HAS2_Wifi::Loop()', 'void HAS2_Wifi::Loop(void',
    'void HAS2_Wifi::LoopFresh(', 'bool HAS2_Wifi::JsonParsingFresh(',
    'bool HAS2_Wifi::HttpRequest('])
# Host String is a std::string adapter; use its C-string for ArduinoJson parsing.
functions = functions.replace('deserializeJson(snapshot, json)', 'deserializeJson(snapshot, json.c_str())')
source = r'''
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <ArduinoJson.h>
#include "has2_http_body.h"
struct String : std::string {
    using std::string::string;
    String() = default;
    String(const std::string &s):std::string(s) {}
    String(int n):std::string(std::to_string(n)) {}
    bool concat(char c) { push_back(c); return true; }
    bool equalsIgnoreCase(const char *text) const {
        std::string other(text);
        if(size()!=other.size()) return false;
        for(size_t i=0;i<size();++i) if(std::tolower((unsigned char)(*this)[i])!=std::tolower((unsigned char)other[i])) return false;
        return true;
    }
    bool startsWith(const char *prefix) const { return find(prefix)==0; }
    String substring(size_t pos) const { return substr(pos); }
};
uint32_t now=0;
uint32_t beginDelay=0, getDelay=0, closeDelay=0;
unsigned long millis() { return now; }
void delay(unsigned long ms) { now+=uint32_t(ms); }
struct Logger { template<class T> void println(T) {} } logger;
Logger *_has2DebugPrint=&logger;
void HAS2DebugPrintf(const char*,...) {}
StaticJsonDocument<2048> my,tag;
StaticJsonDocument<512> shift_machine;
struct Esp { int restarts=0; void restart(){++restarts;} } ESP;
struct Response {
    int status=200;
    String body;
    String length;
    String encoding;
    bool keepOpen=false;
    uint32_t dripMs=0;
};
struct Stream {
    Response response;
    size_t position=0;
    uint32_t started=0;
    bool stopped=false;
    int available() {
        if(stopped) return 0;
        size_t released=response.dripMs?std::min(response.body.size(),size_t(uint32_t(now-started)/response.dripMs)):response.body.size();
        return int(released-position);
    }
    int fd() const { return -1; }
    void stop() { stopped=true; }
    bool connected() { return !stopped && (response.keepOpen || position<response.body.size()); }
    int read() { if(!available()) return -1; return static_cast<unsigned char>(response.body[position++]); }
};
struct Http {
    std::vector<Response> replies;
    size_t consumed=0;
    Stream stream;
    int legacyReads=0,ends=0;
    bool reuse=true,headersCollected=false;
    void setReuse(bool value){reuse=value;}
    void begin(String) { now+=beginDelay; }
    void collectHeaders(const char **headers,size_t count){assert(count==2 && String(headers[0])=="Transfer-Encoding" && String(headers[1])=="Content-Length"); headersCollected=true;}
    int GET(){now+=getDelay;assert(consumed<replies.size());stream=Stream{};stream.response=replies[consumed++];stream.started=now;return stream.response.status;}
    String header(const char *name){assert(headersCollected);return String(name)=="Content-Length"?stream.response.length:stream.response.encoding;}
    bool hasHeader(const char *name){return !header(name).empty();}
    Stream* getStreamPtr(){return &stream;}
    String getString(){++legacyReads;return stream.response.body;}
    String errorToString(int){return "transport failed";}
    void end(){now+=closeDelay;++ends;if(!reuse){assert(stream.stopped);stream.stopped=true;}}
} http;
constexpr int HTTP_CODE_OK=200;
int legacyParses=0,callbacks=0,updates=0;
void callback(){++callbacks;}
class HAS2_Wifi {
    String server="http://test",my_mac="test",HOST_NAME="http://test";
    // INITIALIZERS
    // HOOK_INITIALIZER
    void MaintainWifi(){}
    void FirmwareUpdate(String,String){++updates;}
    void JsonParsing(String,String){++legacyParses;}
public:
    void EnableBoundedResponseBody();
    void SetHttpDiagnosticHook(void (*)(const char *, const char *, int32_t, uint32_t, uint32_t));
    void Receive(String);
    bool HttpRequest(String,String,bool=false);
    bool JsonParsingFresh(String,String);
    void Loop(); void Loop(void(*)(void)); void LoopFresh(void(*)(void));
    void ReceiveMine(){HttpRequest("ReceiveMine","test");}
    void Send(String,String,String){}
};
'''
source = source.replace('// INITIALIZERS', '\n'.join(re.search(r'bool '+name+r' = (?:true|false);',header).group() for name in ['boundedResponseBodyEnabled','freshReceivePending']))
source = source.replace('    // HOOK_INITIALIZER', re.search(r'void \(\*httpDiagnosticHook\).*? = nullptr;', header).group())
source += function(cpp,'struct HAS2BodyClock')+';\n'+functions+r'''
Response response(String body, bool keepOpen=false) {
    Response r; r.body=body; r.length=std::to_string(body.size()); r.keepOpen=keepOpen; return r;
}
void queue(std::vector<Response> replies) { http.replies=replies;http.consumed=0; }
void reset(){http=Http{}; now=0;my.clear();tag.clear();shift_machine.clear();ESP.restarts=callbacks=updates=legacyParses=0;}
bool send(HAS2_Wifi &wifi,Response r) {queue({r});return wifi.HttpRequest("Send","test");}
struct DiagnosticEvent { std::string request,phase; int32_t result; uint32_t elapsed,detail; int ends; };
std::vector<DiagnosticEvent> diagnosticEvents;
void diagnosticHook(const char *request,const char *phase,int32_t result,uint32_t elapsed,uint32_t detail) {
    diagnosticEvents.push_back({request,phase,result,elapsed,detail,http.ends});
}
void checkDiagnostic(HAS2_Wifi &wifi,const char *operation,Response reply,bool success,int32_t bodyResult,uint32_t bytes,uint32_t bodyMs=0) {
    diagnosticEvents.clear();
    const int endsBefore=http.ends;
    queue({reply});
    assert(wifi.HttpRequest(operation,"http://private/card_id_not_for_logs",true)==success);
    const char *phases[]={"request.begin","get.end","body.end","close.begin","close.end","request.end"};
    assert(diagnosticEvents.size()==6);
    for(size_t i=0;i<6;++i) {
        const auto &event=diagnosticEvents[i];
        assert(event.request==operation && event.phase==phases[i]);
        assert(event.ends==endsBefore+(i>=4?1:0));
        assert(event.detail==(i<2?0:bytes));
    }
    assert(diagnosticEvents[0].result==0 && diagnosticEvents[0].elapsed==0);
    assert(diagnosticEvents[1].result==reply.status && diagnosticEvents[1].elapsed==20);
    assert(diagnosticEvents[2].result==bodyResult && diagnosticEvents[2].elapsed==20+bodyMs);
    assert(diagnosticEvents[3].result==reply.status && diagnosticEvents[3].elapsed==20+bodyMs);
    assert(diagnosticEvents[4].result==reply.status && diagnosticEvents[4].elapsed==31+bodyMs);
    assert(diagnosticEvents[5].result==(success?1:0) && diagnosticEvents[5].elapsed==31+bodyMs);
    assert(http.stream.stopped);
}
int main(){
    reset();HAS2_Wifi legacy;
    assert(send(legacy,response("ok")) && http.legacyReads==1 && http.reuse && !http.headersCollected);
    std::cout<<"PASS opt-in default preserves legacy body path\n";
    reset();HAS2_Wifi wifi;wifi.EnableBoundedResponseBody();
    assert(send(wifi,response("ok",true)) && http.legacyReads==0 && !http.reuse && http.stream.stopped);
    assert(send(wifi,response("",true)));
    queue({response("{\"device_state\":\"activate\",\"game_state\":\"activate\"}",true)});
    assert(wifi.HttpRequest("ReceiveMine","test") && String(my["device_state"]|"")=="activate" && legacyParses==0);
    queue({response("{\"player\":1}",true)});assert(wifi.HttpRequest("Receive","test") && tag["player"]==1);
    const String previous=my["device_state"]|"";
    queue({response("{\"device_state\":\"setting\",",true)});
    assert(!wifi.HttpRequest("ReceiveMine","test") && String(my["device_state"]|"")==previous);
    std::cout<<"PASS complete body, empty send, validated JSON and transactional snapshots\n";
    Response r=response("partial",true);r.length="100";
    uint32_t start=now;assert(!send(wifi,r) && uint32_t(now-start)==1500 && http.stream.stopped);
    r.status=503; start=now;assert(!send(wifi,r) && uint32_t(now-start)==1500);
    r.keepOpen=false;start=now;assert(!send(wifi,r) && now==start);
    r=response("partial",true);r.length="";start=now;assert(!send(wifi,r)&&uint32_t(now-start)==1500);
    r.keepOpen=false;assert(send(wifi,r));
    r=response("",false);r.length="";assert(send(wifi,r));
    std::cout<<"PASS incomplete Content-Length, error body stall, EOF-delimited body\n";
    r=response("12345678901234567890",true);r.dripMs=100;start=now;assert(!send(wifi,r)&&uint32_t(now-start)==1500);
    now=UINT32_MAX-500;start=now;assert(!send(wifi,r)&&uint32_t(now-start)==1500);
    r=response(String(16385,'x'));assert(!send(wifi,r));
    r.length="";assert(!send(wifi,r));
    r=response(String(16384,'x'));assert(send(wifi,r));
    for(const char *length:{"-1","abc","1x","99999999999999999999999999"}){r=response("x");r.length=length;assert(!send(wifi,r));}
    std::cout<<"PASS slow drip total deadline, rollover, byte cap, invalid length\n";
    r=response("4;foo=bar\r\nWiki\r\n5\r\npedia\r\n0\r\nTrailer: yes\r\n\r\n",true);r.length="";r.encoding="ChUnKeD";
    queue({r});http.GET();HAS2BodyClock clock;String out;
    assert(Has2HttpBody::readResponse(http,out,clock)==Has2HttpBody::Result::Ok&&out=="Wikipedia");
    assert(send(wifi,r));
    r.body="7\r\npartial\r\n";start=now;assert(!send(wifi,r)&&uint32_t(now-start)==1500);
    r.keepOpen=false;assert(!send(wifi,r));
    for(const char *body:{"x\r\n", "1\r\naX\n0\r\n\r\n", "4001\r\n", ";x\r\n", "0\r\n"}){r.body=body;assert(!send(wifi,r));}
    r.body="0\r\n\r\n";r.length="0";assert(!send(wifi,r));
    r.length="";r.encoding="gzip";assert(!send(wifi,r));
    std::cout<<"PASS chunk decoding, extensions/trailers, truncated chunks, conflicting framing\n";
    // The actual void Receive() API is used by RFID callers, which inspect tag
    // without a success return. A failed lookup for a new card must revoke the
    // previous card's cached role before every transport/parser failure path.
    const Response player=response("{\"device_name\":\"G1P1\",\"role\":\"player\"}");
    std::vector<Response> failedTags;
    for(const char *body:{"{\"role\":\"player\",", "null", "[]", ""})
        failedTags.push_back(response(body));
    r=response("{\"role\":\"player\"}");r.status=503;failedTags.push_back(r);
    r=response("");r.status=-1;failedTags.push_back(r);
    r=response("partial",true);r.length="100";failedTags.push_back(r);
    r.keepOpen=false;failedTags.push_back(r);
    r=response("{\"role\":\"player\"}",true);r.dripMs=100;failedTags.push_back(r);
    r=response("x");r.length="invalid";failedTags.push_back(r);
    r=response("x");r.encoding="gzip";failedTags.push_back(r);
    for(const Response &failure:failedTags) {
        queue({player});wifi.Receive("G1P1");
        assert(String(tag["role"]|"")=="player");
        const uint32_t before=now;
        queue({failure});wifi.Receive("G1T1");
        assert(String(tag["role"]|"")!="player" && tag.isNull());
        assert(uint32_t(now-before)<=1500 && http.stream.stopped);
        queue({player});wifi.Receive("G1P1");
        assert(String(tag["role"]|"")=="player" && String(tag["device_name"]|"")=="G1P1");
    }
    queue({response("{\"device_name\":\"G1T1\",\"role\":\"tagger\"}")});
    wifi.Receive("G1T1");assert(String(tag["role"]|"")=="tagger");
    queue({response("null")});assert(!wifi.HttpRequest("ReceiveMine","test"));
    assert(String(tag["role"]|"")=="tagger" && String(my["device_state"]|"")==previous);
    // Legacy opt-out retains its historical transport-failure behavior.
    queue({failedTags[5]});legacy.Receive("G1T2");
    assert(String(tag["role"]|"")=="tagger");
    std::cout<<"PASS actual Receive revokes stale card role across 11 failures, restores fresh role, preserves ReceiveMine and legacy opt-out\n";
    // Failure must never replay a cached watchdog or dispatch stale my state.
    shift_machine["watchdog"]=1;shift_machine["shift_machine"]=1;
    r=response("{\"watchdog\":0",true);r.length="100";
    queue({r});wifi.Loop(callback);assert(!callbacks&&!ESP.restarts&&http.consumed==1);
    queue({r});wifi.Loop();assert(!ESP.restarts&&http.consumed==1);
    Response changed=response("{\"shift_machine\":1,\"watchdog\":0}");
    Response idle=response("{\"shift_machine\":0,\"watchdog\":0}");
    r=response("{\"device_state\":\"setting\",",true);r.length="100";
    queue({changed,r});wifi.Loop(callback);assert(!callbacks&&String(my["device_state"]|"")==previous);
    queue({idle,response("{\"device_state\":\"activate\",\"game_state\":\"activate\"}")});
    wifi.Loop(callback);assert(callbacks==1&&http.consumed==2&&!ESP.restarts);
    queue({idle});wifi.Loop(callback);assert(callbacks==1&&http.consumed==1);
    assert(http.legacyReads==0);
    std::cout<<"PASS actual opt-in polling suppresses stale watchdog/callback, retries consumed notification\n";
    // Production setter + request implementation: phase ordering, cleanup and
    // final success are observable without URLs, card IDs or response contents.
    assert(diagnosticEvents.empty()); // Hook stays off throughout existing cases.
    beginDelay=7;getDelay=13;closeDelay=11;
    wifi.SetHttpDiagnosticHook(diagnosticHook);
    Response valid=response("{\"device_state\":\"activate\",\"game_state\":\"activate\"}");
    checkDiagnostic(wifi,"ReceiveMine",valid,true,0,uint32_t(valid.body.size()));
    Response invalid=response("{");
    checkDiagnostic(wifi,"ReceiveMine",invalid,false,0,1);
    invalid=response("error");invalid.status=503;
    checkDiagnostic(wifi,"ReceiveMine",invalid,false,0,5);
    invalid=response("");invalid.status=-1;
    checkDiagnostic(wifi,"Receive",invalid,false,-1,0);
    invalid=response("partial",true);invalid.length="100";
    checkDiagnostic(wifi,"ReceiveMine",invalid,false,int32_t(Has2HttpBody::Result::Timeout),0,1500);
    invalid.keepOpen=false;
    checkDiagnostic(wifi,"ReceiveMine",invalid,false,int32_t(Has2HttpBody::Result::Truncated),0);
    invalid=response("x");invalid.length="invalid";
    checkDiagnostic(wifi,"ReceiveMine",invalid,false,int32_t(Has2HttpBody::Result::InvalidFraming),0);
    now=UINT32_MAX-10;
    checkDiagnostic(wifi,"ReceiveMine",valid,true,0,uint32_t(valid.body.size()));
    wifi.SetHttpDiagnosticHook(nullptr);diagnosticEvents.clear();
    assert(send(wifi,response("ok")) && diagnosticEvents.empty());
    beginDelay=getDelay=closeDelay=0;
    std::cout<<"PASS optional HTTP hook: actual setter, six phases, total elapsed/rollover, JSON/status/transport/body failures, disable, no payload identifiers\n";
    // Real socket FIN with connected() stuck true reproduces core 3.3.11.
    struct SocketStream {
        int socket;
        String buffered="buffered";
        int fd()const{return socket;}
        bool connected()const{return true;}
        int available(){return int(buffered.length());}
        int read(){assert(!buffered.empty());int c=buffered[0];buffered.erase(0,1);return c;}
    };
    int sockets[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
    SocketStream tcp{sockets[0],""};HAS2BodyClock tcpClock(true);
    assert(tcpClock.peerState(&tcp)==0); // open idle socket, EAGAIN
    tcp.buffered="buffered";close(sockets[1]);
    assert(tcpClock.peerState(&tcp)==0); // FIN must not discard buffered bytes
    Has2HttpBody::Reader<SocketStream,String,HAS2BodyClock> reader(&tcp,out,tcpClock);
    start=now;assert(reader.read(-1,false)==Has2HttpBody::Result::Ok && out=="buffered" && now==start);
    Has2HttpBody::Reader<SocketStream,String,HAS2BodyClock> truncated(&tcp,out,tcpClock);
    assert(truncated.read(1,false)==Has2HttpBody::Result::Truncated && out.empty());
    close(sockets[0]);assert(tcpClock.peerState(&tcp)==-1); // hard error, not successful EOF
    Has2HttpBody::Reader<SocketStream,String,HAS2BodyClock> broken(&tcp,out,tcpClock);
    assert(broken.read(-1,false)==Has2HttpBody::Result::ConnectionError && out.empty());
    std::cout<<"PASS actual TCP adapter: idle, buffered FIN, unknown-length EOF, truncated FIN, hard error\n";
}
'''
with tempfile.TemporaryDirectory(prefix='generator-http-body-') as tmp:
    src,exe=Path(tmp)/'test.cpp',Path(tmp)/'test'
    src.write_text(source)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(LIB),'-I',str(include),str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
