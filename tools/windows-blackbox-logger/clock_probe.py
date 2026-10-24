"""Best-effort clock brackets; PC wall clock accuracy remains unverified."""
from collections import Counter, OrderedDict
from datetime import timedelta
import json
import re
import secrets
import threading
import time
from protocol import TOPIC
from telemetry import parse_utc, utc_now
from timeline import Anchor


class ClockProbe:
    def __init__(self, site_id, publish, save, monotonic=time.monotonic, now=utc_now):
        self.site_id, self.publish, self.save = site_id, publish, save
        self.monotonic, self.now = monotonic, now
        self.pending, self.last = {}, OrderedDict()
        self.lock = threading.Lock()
        self.counts = Counter()

    def challenge(self, batch):
        mono = self.monotonic()
        with self.lock:
            self.pending = {key: value for key,value in self.pending.items() if mono-value[0]<=2}
            if mono-self.last.get(batch.mac, -float('inf'))<60 or len(self.pending)>=256:
                return
            self.last[batch.mac] = mono
            self.last.move_to_end(batch.mac)
            if len(self.last)>1024:
                self.last.popitem(last=False)
            token = secrets.token_hex(16)
            self.pending[token] = (mono,self.now(),batch.mac,batch.ring)
        if not self.publish(batch.ack_topic, ('T1 '+token+'\n').encode('ascii')):
            with self.lock:
                self.pending.pop(token,None)
            self.counts['publish_failed'] += 1

    def receive(self, topic, payload):
        # Called on MQTT thread: bounded parsing, no SQLite/file/network waits.
        mono, received = self.monotonic(), self.now()
        try:
            match = TOPIC.fullmatch(topic)
            if not match or len(payload)>1024:
                raise ValueError('invalid reply')
            data = json.loads(payload)
            expected = {'v','kind','mac','ring','boot','token','uptime_ms','seq_min','seq_max','record_uptime_wraps'}
            if (not isinstance(data,dict) or set(data)!=expected or type(data['v']) is not int or data['v']!=1 or data['kind']!='time_probe' or data['mac']!=match[1] or not isinstance(data['token'],str) or not re.fullmatch('[0-9a-f]{32}',data['token'])):
                raise ValueError('invalid identity')
            with self.lock:
                pending = self.pending.get(data['token'])
                if not pending or data['mac']!=pending[2] or data['ring']!=pending[3]:
                    raise ValueError('unsolicited reply')
                self.pending.pop(data['token'])
            elapsed = mono-pending[0]
            wall_delta = (parse_utc(received)-parse_utc(pending[1])).total_seconds()
            if not 0<=elapsed<=2 or abs(wall_delta-elapsed)>0.05 or wall_delta<0:
                raise ValueError('expired or stepped clock')
            # 50ms guards wall/monotonic sampling; it does not bound PC UTC offset.
            anchor = Anchor(anchor_id='probe:'+data['token'],site_id=self.site_id,mac=data['mac'],ring=data['ring'],boot=data['boot'],uptime_ms64=data['uptime_ms'],seq_min=data['seq_min'],seq_max=data['seq_max'],record_uptime_wraps=data['record_uptime_wraps'],utc_min=(parse_utc(pending[1])-timedelta(milliseconds=50)).isoformat(),utc_max=(parse_utc(received)+timedelta(milliseconds=50)).isoformat(),origin='MQTT roundtrip; device persisted sequence epoch; PC wall clock offset unverified',clock_quality='pc_unverified',max_delta_ms=1800000)
            if data['uptime_ms']//(1<<32)!=data['record_uptime_wraps']:
                raise ValueError('reply epoch does not match sample')
            if self.save(anchor):
                self.counts['accepted'] += 1
            else:
                self.counts['queue_full'] += 1
        except (ValueError,TypeError,KeyError,OverflowError,RecursionError):
            self.counts['rejected'] += 1
