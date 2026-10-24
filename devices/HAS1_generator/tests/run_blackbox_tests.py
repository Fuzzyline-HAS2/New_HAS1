#!/usr/bin/env python3
"""Host policy and actual NeoShow contention tests; no physical RMT timing claim."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
neo = (ROOT / 'neopixel.ino').read_text()
neo = neo[:neo.index('// =================================================================================')]
neo = neo.replace('#include "generator_neopixel_tx.h"', '')
preamble = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstdio>
static constexpr int NeopixelNum=4;
static const int NumPixels[4]={28,12,16,10};
struct Pixel { uint8_t frame=0; uint8_t* getPixels(){return &frame;} } pixels[4];
static unsigned sent=0;static uint8_t last=0;static bool fail=false;
namespace GeneratorNeo { struct Sender {
 bool show(const uint8_t *p,size_t) { ++sent;last=*p;return !fail; }
 int error(){return 4;}
}; }
namespace has1blackbox {
 struct Logger {
  bool allowed=false,held=false;unsigned begins=0,ends=0;
  bool tryBeginLedOutput(){++begins;if(!allowed)return false;assert(!held);held=true;return true;}
  void endLedOutput(){assert(held);held=false;++ends;}
 } blackbox;
}
namespace GeneratorDiagnostic {enum {Neo=1};}
struct DiagnosticScope {DiagnosticScope(int,uint32_t){} void result(int32_t,uint32_t){} };
struct Logger {template<class... T>void printf(const char*,T...) {}} Serial;
#define BREADCRUMB(x) do {} while(0)
'''
tests = r'''
int main(){
 pixels[0].frame=7;NeoShow(0);assert(sent==0 && neoBlackBoxPending[0]);
 pixels[0].frame=9;NeoShow(0);assert(sent==0 && neoBlackBoxPending[0]);
 has1blackbox::blackbox.allowed=true;NeoRetryPending();
 assert(sent==1 && last==9 && !neoBlackBoxPending[0]); // Latest frame replaces old.
 assert(has1blackbox::blackbox.ends==1 && !has1blackbox::blackbox.held);
 NeoRetryPending();assert(sent==1); // Successful flush is not replayed.
 fail=true;NeoShow(1);assert(sent==2 && neoFaultReported[1]);
 NeoRetryPending();assert(sent==2); // Existing transport quarantine unchanged.
 NeoShow(-1);NeoShow(4);assert(sent==2);
 puts("blackbox LED contention tests passed");
}
'''
with tempfile.TemporaryDirectory(prefix='generator-blackbox-') as directory:
    directory=Path(directory)
    source=directory/'led.cpp';source.write_text(preamble+neo+tests)
    for name,cpp in [('policy',ROOT/'tests/blackbox_policy_test.cpp'),('led',source)]:
        binary=directory/name
        subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer',str(cpp),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
