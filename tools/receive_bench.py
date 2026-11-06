#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""RX-only and offline-codec bench runner for the installed restricted image.

No firmware reads/flashing, TX, PTT, storage commands or arbitrary registers.
Native RX sessions retain their device lease and duration guard and stop on fault.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from tools import firstboot_check as check
from tools import native_modem as native

def allowed(cmd,payload):
    if cmd in (1,2,3,9,0x22):return True
    if cmd==0x20:return payload in (b'\x00',b'\x01')
    if cmd==0x21:
        if not payload:return False
        if payload[0] in (0,8):return len(payload)==1
        if payload[0]==1:return len(payload)==17 and payload[1]==1 and payload[-4:]==bytes(4)
        return payload[0] in (3,4,5,6,7) and len(payload)==5
    return False

def open_link(port):
    if not port or '://' in port or port.startswith('udp:'):raise ValueError('Explicit local serial port required')
    if hashlib.sha256(check.DEBUG.read_bytes()).hexdigest()!=check.DEBUG_SHA:raise ValueError('Pinned protocol module changed')
    spec=importlib.util.spec_from_file_location('receive_protocol',check.DEBUG)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    import serial
    class ReceiveLink(module.DebugLink):
        def __init__(self):
            self.port=port;self.timeout=1;self.seq=0;self.packed_shots=None;self.buf=bytearray()
            self.s=serial.Serial(port=None,baudrate=115200,timeout=0.02,write_timeout=3)
            self.s.rts=self.s.dtr=False;self.s.port=port;self.s.open()
        def request(self,cmd,payload=b'',**kw):
            if not allowed(cmd,payload):raise ValueError('RX-only bench command refused before transmission')
            return super().request(cmd,payload,**kw)
    return ReceiveLink()

def p25_status(link):
    status,data=link.request(0x20,b'\x00',busy_wait=0)
    if status:raise ValueError('P25 status refused')
    return dict(line.split('=',1) for line in data.decode('ascii').splitlines() if '=' in line)

def codec(link):
    state=native.status(link)
    if state['owned'] or state['state'] in (1,2,3,4):raise ValueError('Native session is busy')
    link.request(0x20,b'\x01',busy_wait=0)
    print('Running internal P25 audio-codec/framing test; no RF or speaker output.',flush=True)
    end=time.monotonic()+60
    while True:
        r=p25_status(link)
        if r.get('p25.state')!='1':break
        if time.monotonic()>end:raise TimeoutError('Codec test exceeded 60 seconds')
        time.sleep(0.2)
    expected={'p25.state':'2','p25.ok':'1','p25.pcm_crc':'4e158373','p25.air_bytes':'549','p25.pcm_blocks':'18'}
    r['host_reference_matches']=all(r.get(k)==v for k,v in expected.items())
    return r

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port',required=True);ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('mode',choices=('inspect','codec','native-rx'))
    ap.add_argument('--frequency',type=int,default=145500000)
    ap.add_argument('--windows',type=int,default=1)
    ap.add_argument('--duration-ms',type=int,default=2000)
    ap.add_argument('--sync',choices=('p25','inherited'),default='p25')
    args=ap.parse_args()
    if not 1<=args.windows<=120:ap.error('Windows must be 1..120 (at most four minutes of RX windows)')
    native.validate(1,args.frequency,60,args.duration_ms,250,args.sync=='p25',False)
    args.out.mkdir(parents=True,exist_ok=False)
    report={'mode':args.mode,'rf_transmit_requested':False,'passed':False,'sessions':[]}
    started=time.monotonic()
    report['started_utc']=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())
    try:
        with open_link(args.port) as link:
            report['restrictions_before']=check.restrictions(link)
            report['version']=link.version()
            if report['version'].get('git')!='dfbaf20c-p25-firstboot':raise ValueError('Wrong firmware identity')
            report['view_before']=link.view(['core','channel','features','tasks','storage','dmr_stats'])
            report['native_before']=native.status(link)
            report['codec_before']=p25_status(link)
            if args.mode=='codec':
                report['codec']=codec(link)
                report['passed']=report['codec']['host_reference_matches']
            elif args.mode=='native-rx':
                if int(report['view_before']['rx_hz'])!=args.frequency:raise ValueError('Tune RX channel first; frequency mismatch')
                if report['view_before']['state']!='idle':raise ValueError('Receiver must be idle before starting native test')
                print(f'RX capture starting on {args.frequency} Hz, {args.windows} bounded windows.',flush=True)
                capture_deadline=time.monotonic()+args.windows*(args.duration_ms/1000+0.5)+10
                for i in range(args.windows):
                    if i:
                        # Between hardware windows the ordinary FM receiver is
                        # restored. Wait for an ongoing carrier to end instead
                        # of starting a test that its idle admission must deny.
                        while True:
                            v=link.view(['core'])
                            if v.get('state')=='idle':break
                            if v.get('state')!='rx' or time.monotonic()>capture_deadline:
                                raise RuntimeError('Receiver did not return to idle between bounded windows')
                            time.sleep(0.2)
                    r=native.run_session(link,mode=1,frequency=args.frequency,blocks=60,duration=args.duration_ms,
                                         lease=250,sync=int(args.sync=='p25'),allow_rf=False)
                    capture=r.pop('capture')
                    try:r['modem_snapshot']=native.snapshot(link)
                    except Exception as exc:r['snapshot_error']=str(exc)
                    r['captured_records']=len(capture)
                    if 'status' in r:
                        r['state_name']=native.STATES[r['status']['state']];r['reason_name']=native.REASONS[r['status']['reason']]
                    (args.out/f'rx-{i:02d}.jsonl').write_text(''.join(json.dumps(c)+'\n' for c in capture))
                    report['sessions'].append(r)
                    (args.out/'bench.json').write_text(json.dumps(report,indent=2)+'\n')
                    print(f'Window {i+1}: {len(capture)} records, {r.get("state_name")}, {r.get("reason_name")}',flush=True)
                    if r.get('error') or not r.get('cleanup_confirmed') or r.get('status',{}).get('state')==6:break
                    time.sleep(0.1)
                report['passed']=len(report['sessions'])==args.windows and all(
                    not r.get('error') and r.get('cleanup_confirmed') and r.get('status',{}).get('state')!=6 for r in report['sessions'])
            else:report['passed']=True
            report['view_after']=link.view(['core','features','tasks','storage','dmr_stats'])
            report['restrictions_after']=check.restrictions(link)
    except BaseException as exc:
        report['error']=f'{type(exc).__name__}: {exc}';raise
    finally:
        report['host_elapsed_seconds']=time.monotonic()-started
        report['finished_utc']=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())
        (args.out/'bench.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    if not report['passed']:raise SystemExit(2)
if __name__=='__main__':main()
