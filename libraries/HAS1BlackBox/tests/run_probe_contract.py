#!/usr/bin/env python3
"""Actual C++ time encoder -> collector decoder. No live ESP32/MQTT claim."""
from datetime import datetime, timedelta, timezone
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools/windows-blackbox-logger'))
from clock_probe import ClockProbe
from protocol import Batch
from telemetry import TelemetryStore

CPP = r'''
#include "BlackBoxEvidence.h"
#include <cstdio>
int main(int argc,char **argv) {
  if(argc!=2)return 1;
  char challenge[80],token[33],response[320];
  const int n=std::snprintf(challenge,sizeof(challenge),"T1 %s\n",argv[1]);
  if(n<=0 || !has1blackbox::parseTimeProbe(challenge,size_t(n),token))return 2;
  has1blackbox::ClockCoverage c;
  c.observe(0x0123456789abcdefULL,123,100,0);
  c.observe(0x0123456789abcdefULL,123,200,0);
  const size_t bytes=has1blackbox::encodeTimeProbe(c,"AABBCCDDEEFF",token,123456,response,sizeof(response));
  if(!bytes)return 3;
  std::fwrite(response,1,bytes,stdout);
}
'''
with tempfile.TemporaryDirectory(prefix='blackbox-probe-contract-') as directory:
    temp = Path(directory)
    source, executable = temp / 'probe.cpp', temp / 'probe'
    source.write_text(CPP)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'libraries/HAS1BlackBox/src'), str(source), '-o', str(executable)], check=True)
    sent, mono, wall = [], [100.0], [datetime(2026,10,24,10,tzinfo=timezone.utc)]
    store = TelemetryStore(temp / 'telemetry.sqlite3')
    probe = ClockProbe('store-1', lambda topic,payload: sent.append((topic,payload)) or True,
                       store.save_anchor, monotonic=lambda: mono[0], now=lambda: wall[0].isoformat())
    batch = Batch('iotglove','AABBCCDDEEFF','0123456789abcdef','89abcdef',())
    probe.challenge(batch)
    assert len(sent)==1 and sent[0][0]==batch.ack_topic
    token=sent[0][1].decode('ascii').split()[1]
    response=subprocess.check_output([str(executable),token])
    mono[0]+=0.05
    wall[0]+=timedelta(milliseconds=50)
    probe.receive('has1/blackbox/v1/AABBCCDDEEFF/batch', response)
    assert probe.counts['accepted']==1, probe.counts
    anchors=store.anchor_dicts(site_id='store-1')
    assert len(anchors)==1
    anchor=anchors[0]
    assert anchor['boot']==123 and anchor['seq_min']==100 and anchor['seq_max']==200
    assert anchor['uptime_ms64']==123456 and anchor['record_uptime_wraps']==0
    assert anchor['clock_quality']=='pc_unverified'
    probe.receive('has1/blackbox/v1/AABBCCDDEEFF/batch', response)
    assert probe.counts['rejected']==1
    store.close()
print('PASS C++ challenge/response -> Python UTC bracket -> SQLite; unverified clock and one-shot token retained')
