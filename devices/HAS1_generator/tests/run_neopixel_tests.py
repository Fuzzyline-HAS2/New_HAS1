#!/usr/bin/env python3
"""Exercise the production generator RMT sender with controlled IDF failures."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MOCK = r'''
#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_TIMEOUT=0x107, ESP_ERR_INVALID_ARG=0x102;
using gpio_num_t = int;
constexpr int RMT_CLK_SRC_DEFAULT=0;
union rmt_symbol_word_t {
  struct { uint32_t duration0:15; uint32_t level0:1; uint32_t duration1:15; uint32_t level1:1; };
  uint32_t val;
};
struct Channel;
using rmt_channel_handle_t=Channel*;
using rmt_encoder_handle_t=void*;
struct rmt_copy_encoder_config_t {};
struct rmt_tx_channel_config_t {
  gpio_num_t gpio_num; int clk_src; uint32_t resolution_hz;
  size_t mem_block_symbols, trans_queue_depth; int intr_priority;
  struct { unsigned invert_out:1, with_dma:1, io_loop_back:1, io_od_mode:1, allow_pd:1, init_level:1; } flags;
};
struct rmt_transmit_config_t { int loop_count; struct { unsigned eot_level:1, queue_nonblocking:1; } flags; };
esp_err_t rmt_new_copy_encoder(const rmt_copy_encoder_config_t*, rmt_encoder_handle_t*);
esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t*, rmt_channel_handle_t*);
esp_err_t rmt_enable(rmt_channel_handle_t);
esp_err_t rmt_transmit(rmt_channel_handle_t, rmt_encoder_handle_t, const void*, size_t, const rmt_transmit_config_t*);
esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t, int);
// These APIs are deliberately unavailable: failure cleanup must not enter the
// classic ESP32's unbounded rmt_disable TX_DONE poll or release in-flight data.
'''
TEST = r'''
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
#include "generator_neopixel_tx.h"
struct Channel {
  int pin=0; bool enabled=false, active=false;
  int submitted=0, waited=0; esp_err_t submitResult=ESP_OK, waitResult=ESP_OK;
  const rmt_symbol_word_t* payload=nullptr;
  std::vector<rmt_symbol_word_t> snapshot;
};
static Channel channels[32];
static GeneratorNeo::Sender senders[32];
static int allocated=0, encoderAttempts=0, channelAttempts=0, enableAttempts=0;
static esp_err_t nextEncoder=ESP_OK, nextChannel=ESP_OK, nextEnable=ESP_OK;
static uint32_t fakeMs=0;
static esp_err_t consume(esp_err_t& error) { auto e=error; error=ESP_OK; return e; }
esp_err_t rmt_new_copy_encoder(const rmt_copy_encoder_config_t*, rmt_encoder_handle_t* p) {
  ++encoderAttempts;
  if (nextEncoder) return consume(nextEncoder);
  *p=reinterpret_cast<void*>(uintptr_t(encoderAttempts)); return ESP_OK;
}
esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t* c, rmt_channel_handle_t* p) {
  ++channelAttempts;
  if (nextChannel) return consume(nextChannel);
  assert(c->resolution_hz==10000000 && c->trans_queue_depth==1 && c->mem_block_symbols==64);
  assert(!c->flags.invert_out && !c->flags.with_dma && !c->flags.init_level);
  *p=&channels[allocated++]; (*p)->pin=c->gpio_num; return ESP_OK;
}
esp_err_t rmt_enable(rmt_channel_handle_t c) {
  ++enableAttempts;
  if (nextEnable) return consume(nextEnable);
  c->enabled=true; return ESP_OK;
}
esp_err_t rmt_transmit(rmt_channel_handle_t c, rmt_encoder_handle_t encoder,
                       const void* bytes, size_t size, const rmt_transmit_config_t* conf) {
  assert(c->enabled && encoder && !c->active);
  assert(conf->loop_count==0 && conf->flags.queue_nonblocking && !conf->flags.eot_level);
  ++c->submitted;
  if(c->submitResult) return c->submitResult;
  c->active=true;
  c->payload=static_cast<const rmt_symbol_word_t*>(bytes);
  c->snapshot.assign(c->payload,c->payload+size/sizeof(rmt_symbol_word_t));
  return ESP_OK;
}
esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t c, int ms) {
  assert(c->active && ms==100); ++c->waited;
  fakeMs+=c->waitResult ? uint32_t(ms) : 1u;
  if(c->waitResult) return c->waitResult;
  c->active=false; return ESP_OK;
}
static void assertUnchanged(const Channel& c) {
  assert(!std::memcmp(c.payload,c.snapshot.data(),c.snapshot.size()*sizeof(rmt_symbol_word_t)));
}
static void checkReset(const rmt_symbol_word_t& s) {
  assert(!s.level0 && !s.level1 && s.duration0==1500 && s.duration1==1500);
}
int main() {
  uint8_t bytes[84];
  for(size_t i=0;i<sizeof(bytes);++i) bytes[i]=uint8_t(i*37);
  // Exact MSB-first GRB bytes supplied by Adafruit, no color/brightness rewrite.
  assert(senders[0].begin(25)); auto& normal=channels[0];
  assert(senders[0].show(bytes,sizeof(bytes)));
  assert(normal.snapshot.size()==674);
  checkReset(normal.snapshot.front()); checkReset(normal.snapshot.back());
  for(size_t i=0;i<sizeof(bytes)*8;++i) {
    const auto& s=normal.snapshot[i+1]; bool one=bytes[i/8]&(0x80>>(i%8));
    assert(s.level0 && !s.level1 && s.duration0==(one?8:4) && s.duration1==(one?4:8));
  }
  assert(senders[0].show(bytes,3)); // Back-to-back calls each include reset/latch.
  assert(normal.snapshot.size()==26); checkReset(normal.snapshot.front()); checkReset(normal.snapshot.back());
  assert(senders[0].begin(25)); assert(allocated==1 && encoderAttempts==1);
  std::cout<<"PASS normal timing, latch, maximum frame, repeated begin\n";

  // Lost completion/interrupt: one finite wait, never mutate or clean up the
  // possibly active transfer; unrelated strips still work.
  assert(senders[1].begin(26)); auto& lost=channels[1]; lost.waitResult=ESP_ERR_TIMEOUT;
  uint32_t start=fakeMs;
  assert(!senders[1].show(bytes,36)); assert(fakeMs-start==100);
  assert(senders[1].error()==ESP_ERR_TIMEOUT);
  std::memset(bytes,0xff,sizeof(bytes)); assertUnchanged(lost);
  for(int i=0;i<100;++i) assert(!senders[1].show(bytes,36));
  assert(!senders[1].begin(26)); assert(lost.submitted==1 && lost.waited==1);
  assertUnchanged(lost); assert(senders[0].show(bytes,84));
  assert(senders[2].begin(27)); assert(senders[2].show(bytes,48));
  assert(senders[3].begin(14)); assert(senders[3].show(bytes,30));
  std::cout<<"PASS lost completion bounded, immutable payload, independent strips\n";

  // A late completion cannot accidentally re-enable a quarantined strip or
  // cause stale completion to acknowledge a later transaction.
  lost.active=false; lost.waitResult=ESP_OK;
  assert(!senders[1].show(bytes,36)); assert(lost.submitted==1); assertUnchanged(lost);
  std::cout<<"PASS late completion remains quarantined until reboot\n";

  assert(senders[4].begin(4)); auto& full=channels[4]; full.submitResult=ESP_FAIL;
  start=fakeMs; assert(!senders[4].show(bytes,3));
  assert(fakeMs==start && full.waited==0 && full.submitted==1);
  assert(!senders[4].show(bytes,3)); assert(full.submitted==1);
  std::cout<<"PASS queue failure nonblocking and quarantined\n";

  assert(senders[5].begin(5)); auto& failed=channels[5]; failed.waitResult=ESP_FAIL;
  assert(!senders[5].show(bytes,3)); assert(!senders[5].show(bytes,3)); assertUnchanged(failed);
  std::cout<<"PASS completion error retains in-flight storage without cleanup\n";

  // Relative driver deadline remains bounded across uint32 clock wrap.
  assert(senders[6].begin(6)); auto& wrap=channels[6]; wrap.waitResult=ESP_ERR_TIMEOUT;
  fakeMs=UINT32_MAX-49; start=fakeMs;
  assert(!senders[6].show(bytes,3)); assert(uint32_t(fakeMs-start)==100);
  std::cout<<"PASS finite deadline across clock rollover\n";

  int ea=encoderAttempts, ca=channelAttempts, en=enableAttempts;
  nextEncoder=ESP_FAIL; assert(!senders[7].begin(7)); assert(!senders[7].begin(7));
  assert(encoderAttempts==ea+1 && channelAttempts==ca && enableAttempts==en);
  nextChannel=ESP_FAIL; assert(!senders[8].begin(8)); assert(!senders[8].begin(8));
  assert(encoderAttempts==ea+2 && channelAttempts==ca+1 && enableAttempts==en);
  nextEnable=ESP_FAIL; assert(!senders[9].begin(9)); assert(!senders[9].begin(9));
  assert(encoderAttempts==ea+3 && channelAttempts==ca+2 && enableAttempts==en+1);
  for(int i=7;i<10;++i) assert(!senders[i].show(bytes,3));
  std::cout<<"PASS partial initialization failures allocate at most once\n";

  assert(!senders[10].show(bytes,3));
  assert(senders[10].begin(10)); assert(!senders[10].show(nullptr,3));
  assert(senders[11].begin(11)); assert(!senders[11].show(bytes,85));
  assert(senders[12].begin(12)); assert(!senders[12].show(bytes,0));
  std::cout<<"PASS invalid inputs rejected before transmission\n";
}
'''
with tempfile.TemporaryDirectory(prefix='generator-neopixel-') as temp:
    folder=Path(temp)
    (folder/'driver').mkdir()
    (folder/'driver/rmt_tx.h').write_text(MOCK)
    (folder/'test.cpp').write_text(TEST)
    exe=folder/'test'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                    '-I',str(folder),'-I',str(ROOT),str(folder/'test.cpp'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
