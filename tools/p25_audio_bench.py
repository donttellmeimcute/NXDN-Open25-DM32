#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded on-radio decode/replay/RX playback. No TX, flashing or settings writes."""
import argparse
import json
from pathlib import Path
import struct
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from tools import firstboot_check as check
from tools import receive_bench as receive
FIELDS='version mode state loaded expected playback records cai_frames muted erasures pcm_blocks pcm_crc nonzero fec_max_us imbe_max_us record_max_us decode_us callbacks played played_nonzero queued underruns overflows high_water native_session native_state native_reason gaps unsupported stack_free bad_nids truncated_frames'.split()
PLAYBACK_FIELDS='version paced chunks full_refusals late min_gap_us max_gap_us burst_pairs discarded_blocks generation_drops queued_samples first_push_us last_push_us flags audio_control driver_blocks'.split()

def allowed(cmd,p):
    if cmd in (1,2,3,9,0x22):return cmd!=0x22 or p==b'\x00'
    if cmd==0x21:return p in (b'\x00',b'\x08',b'\x09')
    if cmd!=0x20 or not p:return False
    if p[0] in (0,1,6,7,10,11):return len(p)==1
    if p[0] in (9,12):return len(p)==2 and p[1]<=1
    if p[0]==2:return len(p)==3 and 1<=int.from_bytes(p[1:],'little')<=64
    if p[0]==3:return len(p)==40 and int.from_bytes(p[1:3],'little')<64 and p[3]<=2
    if p[0]==4:return len(p)==3 and p[1]<=1 and p[2]<=15
    if p[0] in (5,8):
        return len(p)==11 and int.from_bytes(p[1:5],'little')>0 and (1000<=int.from_bytes(p[5:7],'little')<=60000 if p[0]==5 else 1<=int.from_bytes(p[5:7],'little')<=300) and int.from_bytes(p[7:9],'little')<=0xfff
    return False

def open_link(port):
    # Reuse the hash-pinned explicit-port transport, with a tighter operation gate.
    link=receive.open_link(port)
    parent=type(link).__mro__[1].request
    def request(cmd,payload=b'',**kw):
        if not allowed(cmd,payload):raise ValueError('P25 audio bench operation refused before transmission')
        return parent(link,cmd,payload,**kw)
    link.request=request
    return link

def status(link):
    _,data=link.request(0x20,b'\x07',busy_wait=0)
    if len(data)!=128:raise ValueError('P25 audio status length mismatch')
    result=dict(zip(FIELDS,struct.unpack('<32I',data)))
    if result['version']!=1:raise ValueError('P25 audio status version mismatch')
    result['pcm_crc_hex']=f'{result["pcm_crc"]:08x}'
    return result

def capture(path):
    rows=[json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    if not 1<=len(rows)<=64:raise ValueError('Replay requires 1..64 captured records')
    for i,row in enumerate(rows):
        if len(bytes.fromhex(row['raw_hex']))!=36 or int(row['rx_type_info'],0)&3==3:
            raise ValueError('Malformed or unsupported raw record')
        if i and (row['session']!=rows[i-1]['session'] or row['sequence']!=((rows[i-1]['sequence']+1)&0xffffffff)):
            raise ValueError('Replay cannot join a capture across a gap')
    return rows

def event_status(link):
    try:
        _,raw=link.request(0x21,b'\x09',busy_wait=0)
        if len(raw)!=32:raise ValueError('Native input-event status length mismatch')
        result=dict(zip('version last_type last_arg last_ms ptt_consumed cancellations cancel_type cancel_arg'.split(),struct.unpack('<8I',raw)))
        if result['version']!=1:raise ValueError('Native input-event status version mismatch')
        return result
    except Exception as exc:return {'available':False,'error':str(exc)}

def playback_status(link):
    try:
        _,raw=link.request(0x20,b'\x0a',busy_wait=0)
        if len(raw)!=64:raise ValueError('Playback status length mismatch')
        result=dict(zip(PLAYBACK_FIELDS,struct.unpack('<16I',raw)))
        if result['version']!=1:raise ValueError('Playback status version mismatch')
        return result
    except Exception as exc:return {'available':False,'error':str(exc)}

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port',required=True);ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('mode',choices=('inspect','replay','live','stop'))
    ap.add_argument('--capture',type=Path);ap.add_argument('--play',action='store_true')
    ap.add_argument('--delay',type=int,default=0);ap.add_argument('--duration-ms',type=int,default=10000)
    ap.add_argument('--frequency',type=int,default=145500000);ap.add_argument('--nac',type=lambda s:int(s,0),default=0x293)
    ap.add_argument('--group',type=int,default=0);ap.add_argument('--no-wait',action='store_true')
    ap.add_argument('--pacing',choices=('paced','legacy'))
    args=ap.parse_args()
    if args.out.exists():raise FileExistsError(args.out)
    if not 0<=args.delay<=15 or not 1000<=args.duration_ms<=300000 or (args.duration_ms>60000 and args.duration_ms%1000) or not 0<=args.nac<=0xfff or not 0<=args.group<=0xffff:ap.error('Invalid audio test bounds; long RX uses whole seconds, at most five minutes')
    rows=capture(args.capture) if args.mode=='replay' and args.capture else None
    if args.mode=='replay' and rows is None:ap.error('Replay requires --capture')
    started=time.monotonic()
    r={'rf_transmit_requested':False,'persistent_writes_requested':False,'mode':args.mode,
       'started_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())}
    try:
        with open_link(args.port) as link:
            r['restrictions_before']=check.restrictions(link);r['before']=status(link)
            r['view_before']=link.view(['core','tasks','storage'])
            if args.pacing:
                if args.mode not in ('replay','live'):raise ValueError('Pacing requires a replay or live test')
                link.request(0x20,bytes([9,int(args.pacing=='paced')]),busy_wait=0)
            if args.mode=='replay':
                link.request(0x20,b'\x02'+struct.pack('<H',len(rows)),busy_wait=0)
                for i,row in enumerate(rows):
                    link.request(0x20,b'\x03'+struct.pack('<HB',i,int(row['rx_type_info'],0)&3)+bytes.fromhex(row['raw_hex']),busy_wait=0)
                link.request(0x20,bytes([4,int(args.play),args.delay]),busy_wait=0)
            elif args.mode=='live':
                if int(r['view_before']['rx_hz'])!=args.frequency:raise ValueError('Frequency must match current channel')
                long=args.duration_ms>60000
                link.request(0x20,bytes([8 if long else 5])+struct.pack('<IHHH',args.frequency,args.duration_ms//1000 if long else args.duration_ms,args.nac,args.group),busy_wait=0)
            elif args.mode=='stop':link.request(0x20,b'\x06',busy_wait=0)
            end=time.monotonic()+(args.duration_ms/1000+10 if args.mode=='live' else args.delay+30)
            active_deadline=time.monotonic()+2
            while True:
                r['audio']=status(link)
                if r['audio']['state']!=1 or args.mode=='inspect':break
                if args.no_wait:
                    if args.mode!='live' or (r['audio']['native_state']==2 and r['audio']['native_session'] and r['audio']['native_session']!=r['before'].get('native_session')):break
                    if time.monotonic()>active_deadline:raise TimeoutError('Live RX did not become active')
                    time.sleep(0.02);continue
                if time.monotonic()>end:
                    link.request(0x20,b'\x06',busy_wait=0);raise TimeoutError('On-radio audio test exceeded deadline')
                time.sleep(0.1)
            r['restrictions_after']=check.restrictions(link)
            r['view_after']=link.view(['core','tasks','storage'])
            r['native_events']=event_status(link)
            r['playback']=playback_status(link)
            r['pending']=r['audio']['state']==1
            r['passed']=r['audio']['state']==2 if args.mode in ('replay','live') and not args.no_wait else True
            if args.mode=='live' and args.no_wait:r['passed']=r['pending'] and r['audio']['native_state']==2
    except BaseException as exc:
        r['error']=f'{type(exc).__name__}: {exc}';raise
    finally:
        r['finished_utc']=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())
        r['host_elapsed_seconds']=time.monotonic()-started
        args.out.parent.mkdir(parents=True,exist_ok=True)
        with args.out.open('x') as out:json.dump(r,out,indent=2);out.write('\n')
    print(json.dumps({k:v for k,v in r.items() if k not in ('view_before','view_after')},indent=2))
    if not r['passed']:raise SystemExit(2)
if __name__=='__main__':main()
