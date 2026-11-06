#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded RX-only RTOS accounting; optional counters must be built in."""
import argparse
import json
from pathlib import Path
import struct
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from tools import p25_audio_bench as audio
from tools import firstboot_check as check

def snapshot(link):
    _,raw=link.request(0x20,b'\x0b',busy_wait=0)
    fields={}
    for line in raw.decode('ascii').splitlines():
        key,value=line.split('=',1)
        if key in fields:raise ValueError('Duplicate CPU field')
        fields[key]=int(value)
    if fields.get('resource.version')!=1 or fields.get('cpu.available')!=1:
        raise ValueError('This image does not provide CPU accounting')
    return fields

def delta(before,after):
    elapsed=(after['cpu.us']-before['cpu.us'])&0xffffffff
    if not 100000<=elapsed<=60000000 or after['cpu.us']>=3600000000 or after['cpu.us']<before['cpu.us']:
        raise ValueError('CPU window invalid or clock near wrap; restart the measurement image')
    tasks={}
    for key in before:
        if key.startswith('cpu.task.'):
            if key not in after:raise ValueError('Task set changed during measurement')
            dt=(after[key]-before[key])&0xffffffff
            if dt>elapsed+max(1000,elapsed//200):raise ValueError('Counter reset or inconsistent CPU clock')
            tasks[key.removeprefix('cpu.task.')]=100*dt/elapsed
    if {k for k in after if k.startswith('cpu.task.')}!={k for k in before if k.startswith('cpu.task.')}:
        raise ValueError('Task set changed during measurement')
    if 'IDLE' not in tasks:raise ValueError('Idle task is missing')
    return {'window_us':elapsed,'task_percent':tasks,
            'rtos_attributed_busy_percent':100-tasks['IDLE'],
            'irq_time_charged_to_interrupted_task':True,'reporting_overhead_included':True}

def wait_done(link,seconds):
    deadline=time.monotonic()+seconds
    while True:
        s=audio.status(link)
        if s['state']!=1:
            if s['state']!=2:raise ValueError('RX workload failed')
            return s
        if time.monotonic()>deadline:raise TimeoutError('RX workload exceeded deadline')
        time.sleep(0.1)

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port',required=True);ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--workload',choices=('idle','audio-idle','replay'),default='idle')
    ap.add_argument('--seconds',type=int,default=5);ap.add_argument('--repeats',type=int,default=3)
    ap.add_argument('--capture',type=Path);ap.add_argument('--checks',choices=('on','off'))
    args=ap.parse_args()
    if args.out.exists():raise FileExistsError(args.out)
    if not 2<=args.seconds<=30 or not 1<=args.repeats<=10:ap.error('Invalid bounded CPU workload')
    rows=audio.capture(args.capture) if args.workload=='replay' and args.capture else None
    if args.workload=='replay' and rows is None:ap.error('Replay needs a saved capture')
    result={'rf_transmit_requested':False,'persistent_writes_requested':False,'workload':args.workload,
            'started_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'passed':False}
    try:
        with audio.open_link(args.port) as link:
            result['restrictions_before']=check.restrictions(link)
            if audio.status(link)['state']==1:raise ValueError('Another audio session is active')
            config=snapshot(link)
            if args.checks:
                if not config.get('p25.bench'):raise ValueError('Lean images compile per-sample checks out')
                link.request(0x20,bytes([12,int(args.checks=='on')]),busy_wait=0)
            if rows:
                link.request(0x20,b'\x02'+struct.pack('<H',len(rows)),busy_wait=0)
                for i,row in enumerate(rows):
                    link.request(0x20,b'\x03'+struct.pack('<HB',i,int(row['rx_type_info'],0)&3)+bytes.fromhex(row['raw_hex']),busy_wait=0)
            hz=None
            if args.workload=='audio-idle':hz=int(link.view(['core'])['rx_hz'])
            owned=False
            try:
                # Create the audio task and warm the decoder before sampling;
                # task-set changes invalidate a percentage comparison.
                if args.workload=='replay':
                    link.request(0x20,b'\x04\x01\x00',busy_wait=0);owned=True
                    result['warmup']=wait_done(link,5);owned=False
                elif args.workload=='audio-idle':
                    prior=audio.status(link)['native_session']
                    link.request(0x20,b'\x05'+struct.pack('<IHHH',hz,(args.seconds+5)*1000,0x293,0),busy_wait=0);owned=True
                    deadline=time.monotonic()+2
                    while True:
                        s=audio.status(link)
                        if s['state']==1 and s['native_state']==2 and s['native_session']!=prior and s['callbacks']>=2:break
                        if time.monotonic()>deadline:raise TimeoutError('RX did not become active')
                        time.sleep(0.02)
                before=snapshot(link)
                if args.workload=='replay':
                    result['replays']=[]
                    for _ in range(args.repeats):
                        link.request(0x20,b'\x04\x01\x00',busy_wait=0);owned=True
                        result['replays'].append(wait_done(link,5));owned=False
                else:
                    time.sleep(args.seconds)
                after=snapshot(link)
                result.update(before=before,after=after,measurement=delta(before,after))
            finally:
                if owned:
                    link.request(0x20,b'\x06',busy_wait=0);wait_done(link,3)
            result['restrictions_after']=check.restrictions(link)
            result['playback']=audio.playback_status(link)
            result['view_after']=link.view(['core','tasks','storage']);result['passed']=True
    except BaseException as exc:
        result['error']=f'{type(exc).__name__}: {exc}';raise
    finally:
        result['finished_utc']=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())
        args.out.parent.mkdir(parents=True,exist_ok=True)
        with args.out.open('x') as f:json.dump(result,f,indent=2);f.write('\n')
    print(json.dumps({k:result[k] for k in ('passed','workload','measurement')},indent=2))
if __name__=='__main__':main()
