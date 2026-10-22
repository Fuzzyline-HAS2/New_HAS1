#include "has2_bounded_transport.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace Has2BoundedTransport;
static unsigned checks = 0, progress = 0;
static void report() { ++progress; }
static std::vector<Phase> phases;
static void phaseReport(Phase phase) { phases.push_back(phase); }
struct Fake final : detail::Io {
  uint32_t time = 0, base = 0, connectAt = 0, writeAt = 0, readCost = 0, writeCost = 0, closeCost = 0;
  bool connectFail = false, writeFail = false, readFail = false, keepOpen = false;
  bool zeroWrite = false;
  size_t writeLimit = 10000, readLimit = 512, sent = 0, pos = 0;
  unsigned closes = 0;
  std::string request;
  std::vector<std::pair<uint32_t, char>> bytes;
  uint32_t now() override { return time; }
  Connected beginConnect(const char *, uint16_t) override {
    return connectFail ? Connected::Failed : uint32_t(time - base) >= connectAt ? Connected::Ready : Connected::Pending;
  }
  Connected pollConnect() override { return beginConnect(nullptr, 0); }
  int write(const uint8_t *p, size_t n) override {
    time += writeCost;
    if (writeFail) return -2;
    if (zeroWrite) return 0;
    if (uint32_t(time - base) < writeAt) return -1;
    n = std::min(n, writeLimit);
    request.append(reinterpret_cast<const char *>(p), n); sent += n;
    return int(n);
  }
  int read(uint8_t *p, size_t n) override {
    time += readCost;
    if (readFail) return -2;
    n = std::min(n, readLimit);
    size_t got = 0;
    while (got < n && pos < bytes.size() && bytes[pos].first <= uint32_t(time - base)) p[got++] = uint8_t(bytes[pos++].second);
    if (got) return int(got);
    return pos < bytes.size() || keepOpen ? -1 : 0;
  }
  int lastError() const override { return 123; }
  void idle() override { ++time; }
  void close() override { ++closes; time += closeCost; }
  void add(const std::string &s, uint32_t at = 0, uint32_t interval = 0) {
    for (char c : s) { bytes.emplace_back(at, c); at += interval; }
  }
};
static Result run(Fake &io, Options options = {}) {
  options.progressHook = report;
  auto result = detail::run(io, "172.30.1.43", 80, "/has2.php?request=Loop", options);
  assert(io.closes == 1); ++checks;
  return result;
}
static std::string response(const std::string &body, int status = 200) {
  return "HTTP/1.1 " + std::to_string(status) + " OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}
int main() {
  { Fake f; f.add(response("{\"ok\":true}")); auto r=run(f); assert(r.ok()&&r.status==200&&r.payload=="{\"ok\":true}"); assert(f.request.find("Connection: close")!=std::string::npos); }
  { Fake f; f.add(response("conflict",409)); auto r=run(f); assert(r.ok()&&r.status==409&&r.payload=="conflict"); }
  { Fake f; f.add("HTTP/1.0 200 OK\r\n\r\nEOF body"); auto r=run(f); assert(r.ok()&&r.payload=="EOF body"); }
  { Fake f; f.add("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2;ok=yes\r\nab\r\n1\r\nc\r\n0\r\nX-End: yes\r\n\r\n"); auto r=run(f); assert(r.ok()&&r.payload=="abc"); }
  { Fake f; f.connectAt=3000; auto r=run(f); assert(r.error==Error::Deadline&&r.phase==Phase::Connect&&r.timings.totalMs==2000); }
  { Fake f; f.writeAt=3000; auto r=run(f); assert(r.error==Error::Deadline&&r.phase==Phase::Write&&r.timings.totalMs==2000); }
  { Fake f; f.writeLimit=1; f.add(response("x")); auto r=run(f); assert(r.ok()&&r.payload=="x"); }
  { Fake f; f.connectAt=1200; f.writeAt=1800; f.add(response("x"),2100); auto r=run(f); assert(r.error==Error::Deadline&&r.phase==Phase::Header&&r.timings.totalMs==2000); }
  { Fake f; f.add(response("x"),0,600); auto r=run(f); assert(r.error==Error::Deadline&&r.phase==Phase::Header&&r.timings.totalMs==2000); }
  { Fake f; f.add("HTTP/1.1 200 OK"); f.keepOpen=true; auto r=run(f); assert(r.error==Error::Deadline&&r.timings.totalMs==2000); }
  { Fake f; f.add("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\na"); f.keepOpen=true; auto r=run(f); assert(r.error==Error::Deadline&&r.phase==Phase::Body&&r.timings.totalMs==1500&&r.payload.empty()); }
  { Fake f; f.add("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\na"); auto r=run(f); assert(r.error==Error::Truncated&&r.payload.empty()); }
  { Fake f; f.add("HTTP/1.1 200 OK\r\n\r\nunknown"); f.keepOpen=true; auto r=run(f); assert(r.error==Error::Deadline&&r.phase==Phase::Body&&r.payload.empty()); }
  { Fake f; f.connectAt=1600; f.add("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\na"); f.keepOpen=true; auto r=run(f); assert(r.error==Error::Deadline&&r.phase==Phase::Body&&r.timings.totalMs==2000); }
  { Fake f; f.add("HTTP/1.1 200 OK\r\nContent-Length: 999999999999999999999\r\n\r\n"); auto r=run(f); assert(r.error==Error::BodyTooLarge); }
  { Fake f; f.add("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nab"); auto r=run(f); assert(r.error==Error::InvalidHeader); }
  { Fake f; f.add("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n"); auto r=run(f); assert(r.error==Error::InvalidHeader); }
  { Fake f; f.add("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n"); auto r=run(f); assert(r.error==Error::UnsupportedEncoding); }
  { Fake f; f.add("HTTP/1.1 200 OK\r\nX: "+std::string(1024,'a')+"\r\n\r\n"); auto r=run(f); assert(r.error==Error::HeaderTooLarge); }
  { Fake f; f.add("HTTP/1.1 200 OK\r\n"); for(int i=0;i<8;++i) f.add("X: "+std::string(600,'a')+"\r\n"); f.add("\r\n"); auto r=run(f); assert(r.error==Error::HeaderTooLarge); }
  { Fake f; f.add("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\n"); auto r=run(f); assert(r.error==Error::InvalidBody); }
  { Fake f; f.add("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nxXX"); auto r=run(f); assert(r.error==Error::InvalidBody); }
  { Fake f; f.add("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx\r\n0\r\n"); auto r=run(f); assert(r.error==Error::Truncated); }
  { Fake f; f.add(response(std::string(1025,'a'))); Options o;o.maxBodyBytes=1024;o.bodyDeadlineMs=1000; auto r=run(f,o);assert(r.error==Error::BodyTooLarge); }
  { Fake f; f.add("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\n");f.keepOpen=true;Options o;o.maxBodyBytes=1024;o.bodyDeadlineMs=1000;auto r=run(f,o);assert(r.error==Error::Deadline&&r.timings.totalMs==1000); }
  { Fake f;f.base=f.time=UINT32_MAX-100;f.add(response("roll"),400);auto r=run(f);assert(r.ok()&&r.payload=="roll"&&r.timings.totalMs==400); }
  { Fake f;f.base=f.time=UINT32_MAX-100;f.connectAt=3000;auto r=run(f);assert(r.error==Error::Deadline&&r.timings.totalMs==2000); }
  { Fake f;f.connectFail=true;auto r=run(f);assert(r.error==Error::Connect&&r.socketError==123); }
  { Fake f;f.writeFail=true;auto r=run(f);assert(r.error==Error::Write); }
  { Fake f;f.readFail=true;auto r=run(f);assert(r.error==Error::Read); }
  { Fake f;f.add("HTTP/1.1 204 No Content\r\n\r\n");f.keepOpen=true;auto r=run(f);assert(r.ok()&&r.payload.empty()); }
  { Fake f;f.add(response(std::string(16384,'b')));auto r=run(f);assert(r.ok()&&r.payload.size()==16384); }
  { Fake f;f.add("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");for(int i=0;i<3500;++i)f.add("1\r\na\r\n");f.add("0\r\n\r\n");auto r=run(f);assert(r.error==Error::BodyTooLarge&&r.payload.empty()); }
  { Fake f;auto r=detail::run(f,"localhost",80,"/",{});assert(r.error==Error::InvalidRequest&&f.closes==1);++checks; }
  { Fake f;auto r=detail::run(f,"127.0.0.1",80,"/bad\r\nInjected: x",{});assert(r.error==Error::InvalidRequest&&f.closes==1);++checks; }
  { Fake f;std::string path="/"+std::string(2048,'x');auto r=detail::run(f,"127.0.0.1",80,path.c_str(),{});assert(r.error==Error::InvalidRequest&&f.closes==1);++checks; }
  { Fake f;f.writeLimit=1;f.writeCost=50;f.add(response("x"));auto r=run(f);assert(r.error==Error::Deadline&&r.phase==Phase::Write&&r.timings.totalMs==2000); }
  { Fake f;f.closeCost=2100;f.add(response("x"));auto r=run(f);assert(r.error==Error::Deadline&&r.phase==Phase::Close&&r.timings.totalMs==2100); }
  { Fake f;f.add("HTTP/1.1 200 OK\r\nContent-Length : 0\r\n\r\n");auto r=run(f);assert(r.error==Error::InvalidHeader); }
  { Fake f;f.add("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx");Options o;o.maxBodyBytes=1;auto r=run(f,o);assert(r.ok()&&r.payload=="x"); }
  { Fake f;f.add("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nF\r\n");Options o;o.maxBodyBytes=1;auto r=run(f,o);assert(r.error==Error::BodyTooLarge); }
  { Fake f;f.add("HTTP/1.1 200 OK\r\n\r\n{}");Options o;o.requireContentLength=true;auto r=run(f,o);assert(r.error==Error::InvalidHeader); }
  { Fake f;f.add(response(""));Options o;o.requireContentLength=true;auto r=run(f,o);assert(r.error==Error::InvalidHeader); }
  { Fake f;f.add(response("{}",409));Options o;o.requireContentLength=true;o.maxBodyBytes=1024;auto r=run(f,o);assert(r.ok()&&r.status==409&&r.payload=="{}"); }
  { Fake f;f.add(response("ok"));Options o;o.phaseHook=phaseReport;phases.clear();auto r=run(f,o);
    assert(r.ok()&&r.phase==Phase::Done&&r.status==200);
    assert((phases==std::vector<Phase>{Phase::Connect,Phase::Write,Phase::Header,Phase::Body,Phase::Close,Phase::Done})); }
  { Fake f;f.connectAt=3000;Options o;o.phaseHook=phaseReport;phases.clear();auto r=run(f,o);
    assert(r.error==Error::Deadline&&r.phase==Phase::Connect&&r.status==0);
    assert((phases==std::vector<Phase>{Phase::Connect,Phase::Close,Phase::Done})); }
  { Fake f;f.add("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\na");f.keepOpen=true;
    Options o;o.phaseHook=phaseReport;phases.clear();auto r=run(f,o);
    assert(r.error==Error::Deadline&&r.phase==Phase::Body&&r.status==200);
    assert((phases==std::vector<Phase>{Phase::Connect,Phase::Write,Phase::Header,Phase::Body,Phase::Close,Phase::Done})); }
  // Real socket adapter: split writes from the server, partial send/read and FIN.
  {
    int listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(bind(listener,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0);assert(listen(listener,1)==0);
    socklen_t len=sizeof(addr);assert(getsockname(listener,reinterpret_cast<sockaddr*>(&addr),&len)==0);
    std::thread server([&]{int client=accept(listener,nullptr,nullptr);assert(client>=0);char b[4096];assert(recv(client,b,sizeof(b),0)>0);const auto a=response("real");assert(send(client,a.data(),a.size(),0)==ssize_t(a.size()));close(client);});
    auto r=get("127.0.0.1",ntohs(addr.sin_port),"/test");server.join();close(listener);
    assert(r.ok()&&r.payload=="real");++checks;
  }
  assert(progress>0);
  std::cout<<"PASS "<<checks<<" bounded transport cases (production parser + real socket adapter)\n";
}
