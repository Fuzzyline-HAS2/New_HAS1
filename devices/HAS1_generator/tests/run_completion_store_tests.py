#!/usr/bin/env python3
"""Exercise the actual generator NVS adapter with injected storage failures."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
using esp_err_t = int;
using nvs_handle_t = int;
enum { ESP_OK, ESP_ERR_NVS_NOT_FOUND, ESP_FAIL, NVS_READONLY, NVS_READWRITE };
static std::vector<uint8_t> durable, pending;
static bool namespaceExists=false, staged=false, stagedErase=false;
static bool failOpen=false, failLength=false, failRead=false, failWrite=false;
static bool failCommit=false, failErase=false, corruptCommit=false, dropErase=false;
static bool failVerify=false, postCommit=false;
static int writes=0, erases=0, commits=0, handles=0;
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out) {
  assert(strcmp(name,"gen_complete")==0);
  if(failOpen || (failVerify && postCommit)) return ESP_FAIL;
  if(!namespaceExists && mode==NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
  namespaceExists=true; *out=1; ++handles; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t,const char *key,void *out,size_t *size) {
  assert(strcmp(key,"pending")==0);
  if((!out&&failLength)||(out&&failRead))return ESP_FAIL;
  if(durable.empty())return ESP_ERR_NVS_NOT_FOUND;
  if(!out){*size=durable.size();return ESP_OK;}
  if(*size<durable.size())return ESP_FAIL;
  *size=durable.size();memcpy(out,durable.data(),durable.size());return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t,const char *key,const void *in,size_t size) {
  assert(strcmp(key,"pending")==0);++writes;
  if(failWrite)return ESP_FAIL;
  const auto *bytes=static_cast<const uint8_t*>(in);
  pending.assign(bytes,bytes+size);staged=true;stagedErase=false;return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t,const char *key) {
  assert(strcmp(key,"pending")==0);++erases;
  if(failErase)return ESP_FAIL;
  if(durable.empty())return ESP_ERR_NVS_NOT_FOUND;
  staged=true;stagedErase=true;return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) {
  ++commits;
  if(failCommit)return ESP_FAIL;
  if(staged){
    if(stagedErase){if(!dropErase)durable.clear();}
    else {durable=pending;if(corruptCommit)durable[20]^=1;}
  }
  staged=false;postCommit=true;return ESP_OK;
}
void nvs_close(nvs_handle_t) {--handles;staged=false;pending.clear();}
#include "generator_completion_store.h"
using namespace GeneratorCompletionStore;
void reset() {
  assert(handles==0);durable.clear();pending.clear();namespaceExists=false;
  staged=stagedErase=failOpen=failLength=failRead=failWrite=failCommit=false;
  failErase=corruptCommit=dropErase=failVerify=postCommit=false;
  writes=erases=commits=0;
}
Intent sample() {
  Intent intent={};strcpy(intent.deviceKey,"LG_1");strcpy(intent.mac,"AA:BB:CC:DD:EE:FF");
  strcpy(intent.epoch,"11111111-2222-3333-4444-555555555555");
  intent.revision=UINT32_MAX;intent.target=Target::Repaired;intent.wireCount=4;intent.maximum=3;
  return intent;
}
int main() {
  Intent intent=sample(),restored={};
  assert(load(restored)==LoadResult::Empty);assert(writes==0);
  assert(save(intent));assert(writes==1&&commits==1&&handles==0);
  assert(load(restored)==LoadResult::Ready);
  assert(restored.revision==UINT32_MAX&&restored.target==Target::Repaired&&restored.wireCount==4);
  assert(strcmp(restored.deviceKey,intent.deviceKey)==0&&strcmp(restored.epoch,intent.epoch)==0);
  for(int i=0;i<100;++i)assert(save(intent));
  assert(writes==1);auto other=intent;other.revision=0;assert(!save(other));assert(writes==1);
  assert(clear());assert(load(restored)==LoadResult::Empty);assert(clear());
  puts("PASS commit/reopen restoration, immutable intent, no per-retry writes, verified clear");

  for(int fault=0;fault<6;++fault){
    reset();
    if(fault==0)failOpen=true;
    if(fault==1)failWrite=true;
    if(fault==2)failCommit=true;
    if(fault==3)corruptCommit=true;
    if(fault==4)failVerify=true;
    if(fault==5){namespaceExists=true;failLength=true;}
    assert(!save(intent));assert(handles==0);
  }
  puts("PASS open/write/commit/readback corruption/readback open/length errors never authorize send");

  reset();assert(save(intent));const auto validRecord=durable;
  for(size_t i=0;i<validRecord.size();++i){
    durable=validRecord;durable[i]^=0x01;restored=sample();restored.revision=123;
    assert(load(restored)==LoadResult::Fault&&restored.revision==123);
    assert(!save(intent));
  }
  durable=validRecord;durable[4]=2;
  Detail::put32(durable.data()+Detail::CrcOffset,Detail::crc(durable.data(),Detail::CrcOffset));
  assert(load(restored)==LoadResult::Fault); // unknown version even with matching checksum
  durable=validRecord;durable.pop_back();assert(load(restored)==LoadResult::Fault);
  durable=validRecord;failLength=true;assert(load(restored)==LoadResult::Fault);failLength=false;
  failRead=true;assert(load(restored)==LoadResult::Fault);failRead=false;
  puts("PASS every byte corruption, incompatible version, truncation and read errors fail closed");

  for(int fault=0;fault<5;++fault){
    reset();assert(save(intent));postCommit=false;
    if(fault==0)failOpen=true;
    if(fault==1)failErase=true;
    if(fault==2)failCommit=true;
    if(fault==3)dropErase=true;
    if(fault==4)failVerify=true;
    assert(!clear());assert(handles==0);
    failOpen=failErase=failCommit=dropErase=failVerify=false;
    // A power interruption before erase commit can leave the original intent.
    const auto state=load(restored);
    assert(state==LoadResult::Ready||state==LoadResult::Empty);
    if(state==LoadResult::Ready)assert(restored.revision==intent.revision);
  }
  puts("PASS failed erase/commit/verify blocks new intent and surviving record restores original token");

  reset();auto bad=intent;memset(bad.deviceKey,'x',sizeof(bad.deviceKey));assert(!save(bad));
  bad=intent;bad.mac[2]='x';assert(!save(bad));
  bad=intent;bad.epoch[8]='x';assert(!save(bad));
  bad=intent;bad.target=Target(3);assert(!save(bad));
  bad=intent;bad.wireCount=2;assert(!save(bad));
  bad=intent;bad.maximum=0;assert(!save(bad));assert(writes==0);
  // Caller can quarantine corruption until a verified reset, then clear just this key.
  durable={1,2,3};namespaceExists=true;assert(load(restored)==LoadResult::Fault);
  assert(clear());assert(save(intent));assert(load(restored)==LoadResult::Ready);
  assert(handles==0);
  puts("PASS malformed identity/conditions rejected and explicit safe-boundary recovery uses one key only");
}
'''

with tempfile.TemporaryDirectory(prefix="generator-completion-store-") as directory:
    temp = Path(directory)
    (temp / "nvs.h").write_text("// NVS calls supplied by fault injection harness.\n")
    (temp / "test.cpp").write_text(SOURCE)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(temp),
                    "-I", str(ROOT), str(temp / "test.cpp"), "-o", str(temp / "test")], check=True)
    subprocess.run([str(temp / "test")], check=True)
