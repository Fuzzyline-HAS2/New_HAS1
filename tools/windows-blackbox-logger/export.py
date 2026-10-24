"""Read-only raw export. Time windows refer to collection time, never device UTC."""
import argparse
import base64
import json
from pathlib import Path
import sqlite3
import sys
from protocol import EVENT_NAMES, decode_record, named_values
from telemetry import normalize_utc


def export(database, output, mac=None, ring=None, limit=10000, *, latest=False,
           after=None, window_start=None, window_end=None):
    if type(limit) is not int or not 1 <= limit <= 1000000:
        raise ValueError("limit must be 1..1000000")
    if (window_start is None)!=(window_end is None):
        raise ValueError("both window boundaries required")
    if window_start:
        window_start,window_end = normalize_utc(window_start),normalize_utc(window_end)
        if window_start>window_end:
            raise ValueError("unordered window")
    context = [mac,ring,latest,window_start,window_end]
    db = sqlite3.connect(Path(database).resolve().as_uri()+"?mode=ro",uri=True,timeout=2)
    try:
        columns = {row[1] for row in db.execute("PRAGMA table_info(records)")}
        times = [name if name in columns else 'NULL AS '+name for name in ('arrival_utc','persisted_utc','queue_latency_ms')]
        where,values = [],[]
        for name,value in (("mac",mac),("ring",ring)):
            if value is not None:
                where.append(name+"=?");values.append(value)
        if window_start:
            where.append("julianday(received_utc) BETWEEN julianday(?) AND julianday(?)")
            values.extend((window_start,window_end))
        ordering = ('received_utc','mac','ring','seq') if latest else ('mac','ring','seq')
        if after:
            try:
                if len(after)>4096: raise ValueError()
                cursor=json.loads(base64.urlsafe_b64decode(after))
                if cursor['context']!=context or len(cursor['key'])!=len(ordering): raise ValueError()
            except Exception as exc:
                raise ValueError('invalid or mismatched cursor') from exc
            where.append('('+','.join(ordering)+')'+('<' if latest else '>')+'('+','.join('?' for _ in ordering)+')')
            values.extend(cursor['key'])
        sql='SELECT mac,ring,seq,device,raw,received_utc,'+','.join(times)+' FROM records'
        if where: sql+=' WHERE '+' AND '.join(where)
        sql+=' ORDER BY '+','.join(name+(' DESC' if latest else '') for name in ordering)+' LIMIT ?'
        rows=db.execute(sql,(*values,limit+1)).fetchall()
        for address,ring_id,seq,device,raw,received,arrival,persisted,latency in rows[:limit]:
            record=decode_record(raw)
            output.write(json.dumps({'mac':address,'ring':ring_id,'seq':seq,'device':device,'received_utc':received,
                'arrival_utc':arrival,'persisted_utc':persisted,'queue_latency_ms':latency,'type':record.kind,
                'event':EVENT_NAMES[record.kind],'boot':record.boot,'uptime_ms':record.uptime_ms,
                'values':record.values,'named_values':named_values(record,device),'text':record.text,'raw_hex':raw.hex()},ensure_ascii=True)+'\n')
        more=len(rows)>limit
        cursor=None
        if more:
            row=rows[limit-1];key=[row[5],*row[:3]] if latest else list(row[:3])
            cursor=base64.urlsafe_b64encode(json.dumps({'context':context,'key':key},separators=(',',':')).encode()).decode()
        return {'report':'raw_export','count':min(len(rows),limit),'truncated':more,'next_cursor':cursor,
                'order':'collection_time_desc' if latest else 'mac_ring_seq','window_basis':'legacy_received_utc (storage processing time), not device event time', 'persisted_utc_semantics':'COMMIT requested; exact commit completion is later' }
    finally: db.close()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('database',type=Path)
    parser.add_argument('--mac');parser.add_argument('--ring')
    parser.add_argument('--limit',type=int,default=10000)
    parser.add_argument('--oldest',action='store_true',help='MAC/ring/seq ascending; default is latest collection first')
    parser.add_argument('--after');parser.add_argument('--window-start');parser.add_argument('--window-end')
    args=parser.parse_args()
    summary=export(args.database,sys.stdout,args.mac,args.ring,args.limit,latest=not args.oldest,
        after=args.after,window_start=args.window_start,window_end=args.window_end)
    print(json.dumps(summary),file=sys.stderr)


if __name__=='__main__': main()
