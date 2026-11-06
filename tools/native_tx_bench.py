#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Single bounded native-modem TX attempt; no automatic retry or fault reset."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import struct
import time
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from tools import firstboot_check as check
from tools import native_modem as native
from tools import normal_reboot
PATTERNS=('plus1','plus3','minus1','minus3','p25-cal','counter')
def allowed(cmd,p,preload=False):
    if cmd in (1,2,9,0x22):return True
    if cmd!=0x21 or not p:return False
    if p[0] in (0,8):return len(p)==1
    if p[0] in (3,4,5):return len(p)==5
    if p[0]==2:return len(p)==43
    if p[0]!=1 or len(p)!=17:return False
    mode,sync,blocks,duration,lease,hz,magic=struct.unpack('<BBHHHII',p[1:])
    return mode==2 and sync==1 and 1<=blocks<=28 and duration==1000 and lease==(1000 if preload else 250) and hz==145500000 and magic==0x3532504e
def open_link(port,preload=False):
    # Same SHA-pinned protocol as health/update tools; no Git installation is
    # needed on the bench PC. Reuse its explicit port and inactive RTS/DTR.
    link=normal_reboot.open_link(port)
    parent=type(link).__mro__[1].request
    link.bench_requests=[]
    def request(cmd,payload=b'',**kw):
        if not allowed(cmd,payload,preload):raise ValueError('Only scoped native TX and health operations permitted')
        start=time.monotonic();row={'cmd':cmd,'op':payload[0] if cmd==0x21 and payload else None}
        try:return parent(link,cmd,payload,**kw)
        except Exception as exc:
            row['error']=type(exc).__name__
            if getattr(exc,'status',None) is None and str(exc).startswith('no answer to command '):
                raise TimeoutError(str(exc)) from exc
            raise
        finally:row['elapsed_ms']=(time.monotonic()-start)*1000;link.bench_requests.append(row)
    link.request=request;link.timeout=0.5
    return link
def attempt(link,data,preload=False):
    if not 1<=len(data)<=28 or any(len(b)!=36 for b in data):
        raise ValueError('Bench attempt requires 1..28 complete 36-byte blocks')
    health=check.collect(link)
    if health['restrictions']['flags']!=47:
        raise ValueError('Explicit bounded native-TX firmware profile is required')
    view=health['view']
    if view.get('state')!='idle' or view.get('mode')!='fm' or view.get('tx_hz')!='145500000':
        raise ValueError('Expected idle FM channel with TX frequency 145.500 MHz')
    before=native.status(link)
    if before['owned'] or before['rx_count'] or before['state'] not in (0,5):
        raise ValueError('Stop/release or explicitly acknowledge the previous native fault first')
    result=native.run_session(link,mode=2,frequency=145500000,blocks=len(data),duration=1000,
        lease=1000 if preload else 250,sync=1,allow_rf=True,data=data,tx_preload=preload)
    result['preloaded']=preload
    result['before_health']=health
    try:
        result['snapshot']=native.snapshot(link)
        result['after_health']=check.collect(link)
    except Exception as exc:
        result['diagnostic_error']=f'{type(exc).__name__}: {exc}'
    result['frequency_hz']=145500000
    result['calibrated_power_index']=0
    result['expected_sha256']=hashlib.sha256(b''.join(data)).hexdigest()
    result['request_timings']=getattr(link,'bench_requests',[])
    if 'status' in result:
        result['state_name']=native.STATES[result['status']['state']]
        result['reason_name']=native.REASONS[result['status']['reason']]
    return result
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port',required=True)
    ap.add_argument('--allow-rf',action='store_true',required=True)
    ap.add_argument('--pattern',choices=PATTERNS,default='plus1')
    ap.add_argument('--blocks',type=int,default=4)
    ap.add_argument('--air',type=Path)
    ap.add_argument('--preload',action='store_true',help='stage a complete air file before one bounded transmission')
    ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args()
    if a.preload and a.air is None:ap.error('--preload requires an explicit air file')
    expected=a.out.with_suffix('.air')
    if a.out.exists() or expected.exists():raise FileExistsError('Preserving existing test output')
    data,pad=native.payloads(a.pattern,a.blocks,a.air.read_bytes() if a.air else None)
    if not 1<=len(data)<=28:ap.error('Maximum 28 blocks per one-second attempt')
    a.out.parent.mkdir(parents=True,exist_ok=True)
    with expected.open('xb') as f:f.write(b''.join(data))
    expected.chmod(0o600)
    with open_link(a.port,a.preload) as link:result=attempt(link,data,a.preload)
    result['non_cai_tail_bytes']=pad
    with a.out.open('x') as f:json.dump(result,f,indent=2);f.write('\n')
    a.out.chmod(0o600)
    print(json.dumps({k:result[k] for k in ('state_name','reason_name','cleanup_confirmed','rf_interoperability_verified') if k in result},indent=2))
    if result.get('error') or result.get('diagnostic_error') or not result['cleanup_confirmed'] or result.get('status',{}).get('state')==6:raise SystemExit(2)
if __name__=='__main__':main()
