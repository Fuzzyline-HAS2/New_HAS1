#!/usr/bin/env python3
"""Completion scheduling and boot-report regressions using production code."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def function(source, signature):
    start = source.index(signature)
    pos = source.index('{', start)
    depth, end = 1, pos + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

crash = (ROOT / 'crash.ino').read_text()
assert 'sheetHttp.POST' not in crash
assert 'SendCrashLogToSheet' not in crash
timer = (ROOT / 'timer.ino').read_text()
assert 'WifiTimer.run()' not in timer
assert 'has2wifi.Loop(' not in timer

source = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include "generator_poll_schedule.h"
bool g_crash_pending = true;
char g_crash_msg[] = "reset reason=TASK_WDT";
bool accepts = false;
unsigned calls = 0;
bool GeneratorNetworkSend(const char *device, const char *column, const char *value) {
  ++calls;
  assert(!strcmp(device,"TG") && !strcmp(column,"crash_log"));
  assert(!strcmp(value,"reset_reason_TASK_WDT"));
  return accepts;
}
'''
source += function(crash, 'static void UrlSafeInPlace') + '\n'
source += function(crash, 'void CrashReportSend') + '\n'
source += r'''
int main() {
  GeneratorPollSchedule s;
  s.begin(0); assert(!s.due(1999)); assert(s.due(2000));
  // A 19-second request completes at 21s. There is no backlog to replay.
  s.completed(21000,true);
  for(unsigned t=21000;t<23000;++t) assert(!s.due(t));
  assert(s.due(23000));
  puts("PASS 19s stall skips missed polls and waits 2s after completion");
  s.completed(23000,false); assert(s.interval()==2000);
  s.completed(25000,false); assert(s.interval()==4000);
  s.completed(29000,false); assert(s.interval()==8000);
  s.completed(37000,false,200); assert(s.interval()==8200);
  s.completed(46000,true,200); assert(s.interval()==2000);
  puts("PASS bounded failure backoff/jitter and successful recovery");
  s.completed(UINT32_MAX-999,true);
  assert(!s.due(999)); assert(s.due(1000));
  s.rejected(1000); assert(!s.due(1249)); assert(s.due(1250));
  puts("PASS rollover and queue rejection cannot create per-loop enqueue flood");
  CrashReportSend(nullptr); CrashReportSend(""); assert(calls==0);
  CrashReportSend("TG"); assert(calls==1 && g_crash_pending);
  accepts=true; CrashReportSend("TG"); assert(calls==2 && !g_crash_pending);
  CrashReportSend("TG"); assert(calls==2);
  puts("PASS queued boot report retains pending on rejection; no external upload");
}
'''
with tempfile.TemporaryDirectory(prefix='generator-poll-test-') as folder:
    d = Path(folder)
    (d / 'test.cpp').write_text(source)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(ROOT), str(d / 'test.cpp'), '-o', str(d / 'test')], check=True)
    subprocess.run([str(d / 'test')], check=True)
    gate = [os.environ.get('CXX', 'c++'), '-x', 'c++', '-fsyntax-only',
            '-include', str(ROOT / 'network_release_gate.h'), '-']
    subprocess.run(gate, input='', text=True, check=True)
    for flags in [['-DGENERATOR_PRODUCTION_BUILD'],
                  ['-DGENERATOR_PRODUCTION_BUILD', '-DGENERATOR_ALLOW_UNFENCED_COMPLETION=1']]:
        result = subprocess.run(gate + flags, input='', text=True, capture_output=True)
        assert result.returncode != 0 and 'not deployable' in result.stderr
    print('PASS production build is blocked even with the legacy bench flag')
