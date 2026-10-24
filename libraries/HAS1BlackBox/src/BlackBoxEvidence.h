#pragma once
#include "BlackBoxRecord.h"
#include <cstdio>
namespace has1blackbox {
// A bounded change detector: first failure is immediate; rapid changes are
// coalesced and the latest pending result is emitted on a later observation.
struct EvidenceGate {
  bool failed = false, seen = false, changedSent = false, pending = false;
  uint32_t lastChanged = 0, lastRepeated = 0;
  int error = 0, phase = 0, status = 0;
  bool observe(uint32_t now, bool bad, int e, int p, int s) {
    const bool transition = !seen ? bad : bad != failed;
    const bool change = bad && (e != error || p != phase || s != status);
    const bool due = bad && uint32_t(now-lastRepeated) >= 60000;
    // Limit transition storms to one detailed exchange per second; first
    // ever failure is never suppressed by normal traffic.
    pending = pending || transition || change || due;
    const bool emit = pending &&
        (!changedSent || uint32_t(now-lastChanged) >= 1000);
    seen = true; failed = bad; error = e; phase = p; status = s;
    if(emit) { pending=false; changedSent=true; lastChanged=lastRepeated=now; }
    return emit;
  }
};
inline uint32_t ipv4FromNetworkBytes(const uint8_t *bytes) {
  return (uint32_t(bytes[0])<<24)|(uint32_t(bytes[1])<<16)|
      (uint32_t(bytes[2])<<8)|uint32_t(bytes[3]);
}
struct ClockCoverage {
  uint64_t ring = 0;
  uint32_t boot = 0, first = 0, last = 0, wraps = 0;
  void observeCurrentRecord(uint64_t r,uint32_t currentBoot,const Record &record,uint32_t wrap) {
    if(get32(record.bytes+8)==currentBoot)
      observe(r,currentBoot,record.seq(),wrap);
  }
  void observe(uint64_t r, uint32_t b, uint32_t seq, uint32_t wrap) {
    if(!seq) return;
    if(ring!=r || boot!=b || wraps!=wrap || !first) {
      ring=r;boot=b;first=seq;wraps=wrap;
    }
    last=seq;
  }
};
inline bool parseTimeProbe(const char *p,size_t n,char (&token)[33]) {
  if(n!=36 || std::memcmp(p,"T1 ",3) || p[35]!='\n') return false;
  for(size_t i=0;i<32;++i) {
    const char c=p[3+i];
    if(!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return false;
    token[i]=c;
  }
  token[32]=0;return true;
}
inline size_t encodeTimeProbe(const ClockCoverage &c,const char *mac,
    const char *token,uint64_t uptime,char *out,size_t cap) {
  if(!c.ring || !c.first || c.last<c.first || (uptime>>32)!=c.wraps) return 0;
  int n=std::snprintf(out,cap,
      "{\"v\":1,\"kind\":\"time_probe\",\"mac\":\"%s\",\"ring\":\"%016llx\","
      "\"boot\":%lu,\"token\":\"%s\",\"uptime_ms\":%llu,\"seq_min\":%lu,"
      "\"seq_max\":%lu,\"record_uptime_wraps\":%lu}",mac,
      (unsigned long long)c.ring,(unsigned long)c.boot,token,
      (unsigned long long)uptime,(unsigned long)c.first,(unsigned long)c.last,
      (unsigned long)c.wraps);
  return n>0 && size_t(n)<cap?size_t(n):0;
}
// RTC evidence is independent from flash ACKs. Original boot/uptime survive a
// warm reset, but sequence identities change when replayed into the flash ring.
// Invalid/torn slots are skipped. Power-loss retention is NOT promised.
struct RetainedEvidence {
  static constexpr size_t Capacity=8;
  uint32_t magic;
  uint8_t entries[Capacity][64];
  uint32_t next;
  void clear() { magic=0x32454242;next=1;std::memset(entries,0,sizeof(entries)); }
  void add(Record r) {
    if(magic!=0x32454242 || !next || next==UINT32_MAX) clear();
    r.sequence(next);
    std::memcpy(entries[(next-1)%Capacity],r.bytes,64);
    ++next;
  }
  size_t recover(Record (&out)[Capacity],bool warm) const {
    if(!warm || magic!=0x32454242) return 0;
    size_t count=0;
    for(const auto &bytes:entries) { Record r;std::memcpy(r.bytes,bytes,64);if(r.valid())out[count++]=r; }
    for(size_t i=1;i<count;++i) {
      Record r=out[i];size_t j=i;
      while(j && out[j-1].seq()>r.seq()) {out[j]=out[j-1];--j;}
      out[j]=r;
    }
    return count;
  }
};
} // namespace has1blackbox
