#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Normal remote restart only; no TX, flash, RAM loading, or firmware reads."""
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

def allowed(cmd,p):
    if cmd in (1,2,9,11):return True
    if cmd in (0x20,0x21,0x22):return p==b'\x00'
    return cmd==13 and len(p)==5 and p[0]==0

def open_link(port,serial_port=None):
    if not port or '://' in port or port.startswith('udp:'):raise ValueError('Explicit local serial port required')
    if hashlib.sha256(check.DEBUG.read_bytes()).hexdigest()!=check.DEBUG_SHA:raise ValueError('Protocol module differs')
    spec=importlib.util.spec_from_file_location('restart_protocol',check.DEBUG)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    import serial
    class RestartLink(module.DebugLink):
        def __init__(self):
            self.port=port;self.timeout=1;self.seq=0;self.packed_shots=None;self.buf=bytearray()
            self.s=serial_port
            if self.s is None:
                self.s=serial.Serial(port=None,baudrate=115200,timeout=0.02,write_timeout=3)
                self.s.rts=self.s.dtr=False;self.s.port=port;self.s.open()
        def request(self,cmd,payload=b'',**kw):
            if not allowed(cmd,payload):raise ValueError('Only health queries and normal restart permitted')
            return super().request(cmd,payload,**kw)
    return RestartLink()

def request_restart(link):
    """Normal restart after health/idle checks. Caller keeps the same port open."""
    check.restrictions(link)
    n=native.status(link)
    if n['owned'] or n['state'] in (1,2,3,4):raise ValueError('Native experiment is busy')
    _,p=link.request(0x20,b'\x00')
    if b'p25.state=1\n' in p:raise ValueError('Codec worker is busy')
    before=link.view(['core']);chip=link.chip()
    link.reboot(0,chip['token'])
    return before

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port',required=True);ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args()
    if args.out.exists():raise FileExistsError(args.out)
    report={'normal_restart_requested':False,'normal_restart_verified':False,'rf_transmit_requested':False,'flash_written':False}
    try:
        with open_link(args.port) as link:
            before=request_restart(link)
            report['normal_restart_requested']=True
            observed=bytearray(link.buf);link.buf.clear()
            end=time.monotonic()+2.5
            while time.monotonic()<end:
                observed.extend(link.s.read(max(1,min(link.s.in_waiting,256))))
            report['boot_rom_greeting_seen']=b'\x02\x24\x00\x03' in observed
        time.sleep(1)
        with check.open_link(args.port) as link:health=check.collect(link)
        report['health']=health
        report['uptime_before_ms']=int(before['ms']);report['uptime_after_ms']=int(health['view']['ms'])
        report['normal_restart_verified']=report['uptime_after_ms']<report['uptime_before_ms']
        if not report['normal_restart_verified']:raise ValueError('Uptime did not reset')
    except BaseException as exc:
        report['error']=f'{type(exc).__name__}: {exc}';raise
    finally:
        args.out.parent.mkdir(parents=True,exist_ok=True)
        with args.out.open('x') as f:json.dump(report,f,indent=2);f.write('\n')
    print(json.dumps({k:v for k,v in report.items() if k!='health'},indent=2))
if __name__=='__main__':main()
