#include "HAS2BoundedHTTP.h"
#include <cassert>
#include <cerrno>
#include <iostream>
#include <sys/socket.h>
uint32_t now, fakeConnectDelay=0; bool fakeConnectOk=true;
uint32_t millis(){return now;}
void delay(uint32_t n){now += std::max(1u,n);}
std::string response; size_t cursor; bool eof,sendStall; uint32_t nextByte,interval;
extern "C" ssize_t send(int,const void*,size_t n,int){if(sendStall){errno=EAGAIN;return -1;}return n;}
extern "C" ssize_t recv(int fd,void *p,size_t n,int flags){
 if(fd<0){errno=EBADF;return -1;}
 if(cursor==response.size()){if(eof)return 0;errno=EAGAIN;return -1;}
 if(int32_t(now-nextByte)<0){errno=EAGAIN;return -1;}
 size_t bytes=interval?1:std::min(n,response.size()-cursor);
 memcpy(p,response.data()+cursor,bytes);
 if(!(flags&MSG_PEEK)){cursor+=bytes;nextByte=now+interval;}
 return bytes;
}
void reset(std::string data,bool close=true,uint32_t drip=0){now=0;fakeConnectDelay=0;fakeConnectOk=true;response=data;cursor=0;eof=close;sendStall=false;nextByte=0;interval=drip;}
HAS2HttpResult get(bool expected){String payload;HAS2HttpResult result;const bool ok=has2BoundedGet("http://127.0.0.1/",payload,result,nullptr,nullptr);assert(ok==expected);assert(result.elapsedMs<=5010);return result;}
int main(){
 reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}"); assert(get(true).bytes==2);
 reset("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"); assert(get(true).bytes==0);
 reset("HTTP/1.1 200 OK\r\n\r\n{}"); assert(get(true).bytes==2);
 reset("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\nX-Test: ok\r\n\r\n"); assert(get(true).bytes==2);
 reset("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n{}",false); assert(get(false).error==HAS2HttpError::Transport);
 reset("HTTP/1.1 200 OK\r\n\r\n{}",false); assert(get(false).error==HAS2HttpError::Transport);
 reset("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n{}"); assert(get(false).error==HAS2HttpError::Transport);
 reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}",false,150); assert(get(false).elapsedMs>=5000);
 reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}"); fakeConnectDelay=9000; assert(get(false).elapsedMs==1500);
 reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}"); sendStall=true; assert(get(false).elapsedMs>=1500);
 reset("HTTP/1.1 200 OK\r\nX-Test: unfinished",false); assert(get(false).error==HAS2HttpError::Transport);
 reset("HTTP/1.1 200 OK\r\nContent-Length: 8193\r\n\r\n"); assert(get(false).error==HAS2HttpError::TooLarge);
 reset("HTTP/1.1 200 OK\r\n\r\n"+std::string(8193,'x')); assert(get(false).error==HAS2HttpError::TooLarge);
 reset("HTTP/1.1 200 OK\r\nX: "+std::string(1100,'x')+"\r\n\r\n"); assert(get(false).error==HAS2HttpError::TooLarge);
 reset("HTTP/1.1 200 OK\r\nContent-Length: xyz\r\n\r\n"); assert(get(false).error==HAS2HttpError::Framing);
 reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n"); assert(get(false).error==HAS2HttpError::Framing);
 reset("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nxyz\r\n"); assert(get(false).error==HAS2HttpError::Framing);
 reset("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n",false); assert(get(false).error==HAS2HttpError::Transport);
 reset("HTTP/1.1 503 Unavailable\r\nContent-Length: 2\r\nRetry-After: 999999999999\r\n\r\n{}");auto retry=get(false);assert(retry.error==HAS2HttpError::HttpStatus&&retry.retryAfterMs==120000);
 reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}",false);now=UINT32_MAX-100;nextByte=now;get(true);
 reset("HTTP/1.1 503 Unavailable\r\n\r\n"); assert(get(false).error==HAS2HttpError::HttpStatus);
 reset("HTTP/1.1 200abc OK\r\n\r\n"); assert(get(false).error==HAS2HttpError::Framing);
 reset("HTTP/1.1 503 Unavailable\r\nContent-Length: 2000\r\nRetry-After: 60\r\n\r\n",false); auto stalled=get(false); assert(stalled.error==HAS2HttpError::HttpStatus&&stalled.retryAfterMs==60000&&stalled.elapsedMs<1500);
 reset("HTTP/1.1 429 Slow\r\nContent-Length: 99999999999999999999\r\nRetry-After: 2\r\n\r\n",false); auto huge=get(false); assert(huge.error==HAS2HttpError::HttpStatus&&huge.retryAfterMs==2000);
 reset("HTTP/1.1 200 OK\r\n"+std::string("X: ")+std::string(1000,'x')+"\r\nX: "+std::string(1000,'x')+"\r\nX: "+std::string(1000,'x')+"\r\nX: "+std::string(1000,'x')+"\r\nX: "+std::string(100,'x')+"\r\n\r\n"); assert(get(false).error==HAS2HttpError::TooLarge);
 reset("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2001\r\n"); assert(get(false).error==HAS2HttpError::TooLarge);
 reset("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0;"+std::string(130,'x')+"\r\n\r\n"); assert(get(false).error==HAS2HttpError::TooLarge);
 reset("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nX: "+std::string(1000,'x')+"\r\nX: "+std::string(1000,'x')+"\r\nX: "+std::string(100,'x')+"\r\n\r\n"); assert(get(false).error==HAS2HttpError::TooLarge);
 reset("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}XX0\r\n\r\n"); assert(get(false).error==HAS2HttpError::Framing);
 reset(""); {HAS2HttpResult r;HAS2DeadlineClient socket(r,nullptr,nullptr);assert(socket.connect("example.invalid",80,1500)==0);assert(r.error==HAS2HttpError::Transport&&now==0);}
 reset(""); {HAS2HttpResult r;String payload;assert(!has2BoundedGet("https://127.0.0.1/",payload,r,nullptr,nullptr));assert(r.error==HAS2HttpError::Transport&&now==0);}
 std::cout<<"bounded HTTP: 31 fault/framing/deadline cases passed\n";
}
