"""Generic bounded CSV import using an operator-supplied column mapping.

No vendor header names are assumed. Mapping keys: message (required), timestamp,
client_mac, ap_mac. Timestamps require explicit offsets; missing timestamp remains
unknown. Original CSV cell values are preserved, not reclassified as wire Syslog.
"""
import argparse
import csv
import hashlib
import io
import json
from dataclasses import replace
from datetime import datetime, timezone
from pathlib import Path
from omada import parse_syslog, to_telemetry
from telemetry import TelemetryStore, normalize_mac, normalize_utc, utc_now


def import_csv(store, filename, site_id, source_ip, mapping, limit=10000):
    if type(limit) is not int or not 1<=limit<=100000 or not isinstance(mapping,dict) or 'message' not in mapping or set(mapping)-{'message','timestamp','client_mac','ap_mac'} or any(not isinstance(v,str) or not v for v in mapping.values()):
        raise ValueError('explicit bounded CSV mapping/limit required')
    path=Path(filename)
    if path.stat().st_size>20*1024*1024:
        raise ValueError('CSV maximum is20MiB')
    raw=path.read_bytes()
    if len(raw)>20*1024*1024: raise ValueError('CSV maximum is20MiB')
    digest=hashlib.sha256(raw).hexdigest()
    csv.field_size_limit(8192)
    reader=csv.DictReader(io.StringIO(raw.decode('utf-8-sig'),newline=''))
    if not reader.fieldnames or len(reader.fieldnames)!=len(set(reader.fieldnames)) or any(name not in reader.fieldnames for name in mapping.values()):
        raise ValueError('missing or duplicate CSV header')
    inserted=duplicates=0
    for number,row in enumerate(reader,1):
        if number>limit: return {'inserted':inserted,'duplicates':duplicates,'truncated':True}
        if None in row or any(value is None for value in row.values()): raise ValueError('malformed CSV row')
        serialized=json.dumps(row,ensure_ascii=False,separators=(',',':'))
        if len(serialized.encode())>8192: raise ValueError('CSV row exceeds8192bytes')
        message=row[mapping['message']]
        event=to_telemetry(parse_syslog(message.encode(),source_ip,datetime.now(timezone.utc),site_id))
        stable_details = {key:value for key,value in event.fields.items() if key not in ("observed_minus_event_seconds", "timestamp_flags", "assumed_utc_offset_minutes")}
        fields=dict(stable_details,csv_columns=row,csv_file_sha256=digest,csv_row=number,csv_mapping=mapping,raw_encoding='utf8_csv_cells')
        occurred=None
        if mapping.get('timestamp') and row[mapping['timestamp']]: occurred=normalize_utc(row[mapping['timestamp']])
        mac=normalize_mac(row[mapping['client_mac']]) if mapping.get('client_mac') and row[mapping['client_mac']] else event.mac
        if mapping.get('ap_mac') and row[mapping['ap_mac']]: fields['ap_mac']=normalize_mac(row[mapping['ap_mac']])
        event=replace(event,event_id='csv:'+digest+':'+str(number),occurred_utc=occurred,clock_quality='source_offset_unverified' if occurred else 'missing',mac=mac,fields=fields,raw=serialized)
        if store.save(event): inserted+=1
        else: duplicates+=1
    return {'inserted':inserted,'duplicates':duplicates,'truncated':False}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input',type=Path);parser.add_argument('--database',required=True)
    parser.add_argument('--site-id',required=True);parser.add_argument('--source-ip',required=True)
    parser.add_argument('--mapping',required=True,help='JSON object mapping semantic names to actual CSV headers')
    parser.add_argument('--limit',type=int,default=10000)
    args=parser.parse_args();store=TelemetryStore(args.database)
    try: print(json.dumps(import_csv(store,args.input,args.site_id,args.source_ip,json.loads(args.mapping),args.limit)))
    finally: store.close()


if __name__=='__main__': main()
