#pragma once
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <stdint.h>
#include <sys/types.h>
#define F(x) x
#define log_d(...) ((void)0)
#define log_w(...) ((void)0)
#define log_v(...) ((void)0)
#define HTTPC_ERROR_CONNECTION_REFUSED -1
#define HTTPC_ERROR_SEND_HEADER_FAILED -2
#define HTTPC_ERROR_NOT_CONNECTED -4
#define HTTPC_ERROR_CONNECTION_LOST -5
#define HTTPC_ERROR_NO_STREAM -6
#define HTTPC_ERROR_NO_HTTP_SERVER -7
#define HTTPC_ERROR_TOO_LESS_RAM -8
#define HTTPC_ERROR_ENCODING -9
#define HTTPC_ERROR_STREAM_WRITE -10
#define HTTPC_ERROR_READ_TIMEOUT -11
#define HTTP_TCP_RX_BUFFER_SIZE 4096
uint32_t millis();
void delay(uint32_t);
class String {
 std::string s;
 public:
 String() = default; String(const char *v) : s(v) {} String(std::string v):s(v){}
 size_t length() const {return s.size();} const char *c_str() const{return s.c_str();}
 char operator[](size_t i) const {return i<s.size()?s[i]:0;}
 String &operator+=(char c){s+=c;return *this;}
 bool concat(const char *p,size_t n){s.append(p,n);return true;}
 bool startsWith(const char *p) const{return s.rfind(p,0)==0;}
 bool endsWith(const char *p) const{std::string q(p);return s.size()>=q.size()&&s.compare(s.size()-q.size(),q.size(),q)==0;}
 void remove(size_t p){s.erase(p);}
 void trim(){auto b=s.find_first_not_of(" \r\n\t");if(b==std::string::npos){s.clear();return;}s=s.substr(b,s.find_last_not_of(" \r\n\t")-b+1);}
 int indexOf(char c,int from=0) const{auto p=s.find(c,from);return p==std::string::npos?-1:int(p);}
 int indexOf(const char *c) const{auto p=s.find(c);return p==std::string::npos?-1:int(p);}
 String substring(int b,int e)const{if(b<0)b=0;if(e<0)e=s.size();return s.substr(b,e-b);}
 String substring(int b)const{return substring(b,s.size());}
 int toInt() const{return std::atoi(s.c_str());}
 bool equalsIgnoreCase(const String &v)const{auto a=s,b=v.s;for(auto &c:a)c=std::tolower(c);for(auto &c:b)c=std::tolower(c);return a==b;}
 bool operator==(const char *v)const{return s==v;}
};
class Stream {
 public:
 virtual ~Stream()=default; virtual size_t write(uint8_t)=0;
 virtual size_t write(const uint8_t*,size_t)=0;
 virtual int available()=0; virtual int read()=0; virtual int peek()=0;
 virtual size_t readBytes(char*,size_t){return 0;}
 virtual size_t readBytes(uint8_t*p,size_t n){return readBytes((char*)p,n);}
 int getWriteError(){return 0;} void clearWriteError(){}
};
class IPAddress { public: bool fromString(const char *p){return std::strcmp(p,"127.0.0.1")==0;} };
extern uint32_t fakeConnectDelay; extern bool fakeConnectOk;
class NetworkClient : public Stream {
 int descriptor=-1;
 public:
 virtual int connect(const char*,uint16_t,int32_t){return 0;}
 int connect(IPAddress,uint16_t,int32_t timeout){delay(std::min(fakeConnectDelay,uint32_t(timeout)));if(fakeConnectDelay>=uint32_t(timeout)||!fakeConnectOk)return 0;descriptor=7;return 1;}
 size_t write(uint8_t)override{return 0;} size_t write(const uint8_t*,size_t)override{return 0;}
 int available()override{return 0;} int read()override{return -1;} int peek()override{return -1;}
 virtual int read(uint8_t*,size_t){return 0;} size_t readBytes(char*,size_t)override{return 0;}
 using Stream::readBytes;
 virtual uint8_t connected(){return descriptor>=0;}
 void stop(){descriptor=-1;} int fd()const{return descriptor;} void setTimeout(uint32_t){}
};
// Only inherited request plumbing is stubbed. Production response-parser and
// socket deadline loops are compiled verbatim by the harness.
class HTTPClient {
 protected:
 struct Header {String key,value;};
 NetworkClient *_client=nullptr; int _size=-1,_returnCode=0; bool _reuse=false,_canReuse=false,_collectAllHeaders=false;
 enum {HTTPC_TE_IDENTITY,HTTPC_TE_CHUNKED}; int _transferEncoding=HTTPC_TE_IDENTITY;
 uint32_t _tcpTimeout=1500; String _location; std::vector<Header> _currentHeaders;
 bool connect(){return _client->connect("127.0.0.1",80,1500);}
 bool sendHeader(const char*){const char *p="GET / HTTP/1.1\r\n\r\n";return _client->write((const uint8_t*)p,strlen(p))==strlen(p);}
 void setCookie(String,String){}
 int returnError(int e){return e;} void disconnect(bool){_client->stop();}
 public:
 bool begin(NetworkClient &client,const String&){_client=&client;return true;}
 bool connected(){return _client && _client->connected();}
 void setReuse(bool v){_reuse=v;} void setTimeout(uint32_t v){_tcpTimeout=v;} void setConnectTimeout(uint32_t){}
 int getSize(){return _size;} void end(){assert(!_client || _client->fd()<0);if(_client)_client->stop();}
 void collectHeaders(const char **p,size_t n){for(size_t i=0;i<n;++i)_currentHeaders.push_back({p[i],{}});}
 String header(const char*p){for(auto &h:_currentHeaders)if(h.key.equalsIgnoreCase(p))return h.value;return {};}
};
