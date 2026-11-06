#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Capture the already-running first-boot display over the cable; queries only."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from tools import firstboot_check as check

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port',required=True);ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args()
    if args.out.exists():raise FileExistsError('Preserving existing screenshot')
    if '://' in args.port or args.port.startswith('udp:'):raise ValueError('Explicit local serial port required')
    if hashlib.sha256(check.DEBUG.read_bytes()).hexdigest()!=check.DEBUG_SHA:raise ValueError('Protocol source differs')
    spec=importlib.util.spec_from_file_location('screen_protocol',check.DEBUG)
    protocol=importlib.util.module_from_spec(spec);spec.loader.exec_module(protocol)
    import serial
    class ScreenLink(protocol.DebugLink):
        def __init__(self):
            self.port=args.port;self.timeout=1;self.seq=0;self.packed_shots=None;self.buf=bytearray()
            self.s=serial.Serial(port=None,baudrate=115200,timeout=0.02,write_timeout=3)
            self.s.rts=self.s.dtr=False;self.s.port=args.port;self.s.open()
        def request(self,cmd,payload=b'',**kw):
            if cmd not in (2,5,0x22):raise ValueError('Only restriction/version/screenshot queries permitted')
            return super().request(cmd,payload,**kw)
    with ScreenLink() as link:
        check.restrictions(link)
        info,data,stable=link.screenshot(tries=3)
        w,h,rows=protocol.to_rgb(info,data,False)
        args.out.parent.mkdir(parents=True,exist_ok=True)
        protocol.write_png(str(args.out),w,h,rows)
    print(json.dumps({'width':w,'height':h,'stable':stable,'output':str(args.out),'rf_requested':False}))
if __name__=='__main__':main()
