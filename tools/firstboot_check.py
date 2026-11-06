#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only health receipt for an already installed first-boot image.

Never installs, reboots, injects keys, keys TX, starts a modem test or runs
the codec diagnostic. No automatic port selection. Not usable on stock firmware.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

ROOT=Path(__file__).resolve().parents[1]
DEBUG=ROOT/'research/upstream/radiofoundry/tools/rfoundry_debug.py'
DEBUG_SHA='b2e572cf6119d1bf1d287a126b05df9233a5dfb4e3ab7e2d8685e3732b92075c'
COUNTERS=('board_tx','pa_key','rf_switch','pa_gpio','flash_write','flash_erase',
          'nor_write_enable','fd_tx','storage_claim','debug_command','native_tx')

def restrictions(link):
    status,data=link.request(0x22,b'\x00',busy_wait=0)
    if status or len(data)!=4*(2+len(COUNTERS)):
        raise ValueError('Missing or malformed first-boot restrictions receipt')
    version,flags,*counts=struct.unpack('<'+'I'*(2+len(COUNTERS)),data)
    if version!=1 or flags not in (31,15,47):raise ValueError('First-boot restrictions/version mismatch')
    return {'version':version,'flags':flags,'native_tx_bench_enabled':flags==47,'denied':dict(zip(COUNTERS,counts))}

def collect(link):
    before=restrictions(link) # Require the immutable restrictions before other commands.
    token=b'FIRSTBOOT-HEALTH'
    status,echo=link.request(1,token,busy_wait=0)
    if status or echo!=token:raise ValueError('Cable echo mismatch')
    version=link.version()
    if 'dfbaf20c-p25-firstboot' not in version.values():raise ValueError('Unexpected firmware identity')
    view=link.view(['core','channel','features','tasks','storage'])
    if view.get('state') not in ('idle','rx','tx_denied'):
        raise ValueError('Unexpected radio state')
    for key in ('storage.sector_writes','storage.claim_pending'):
        if str(view.get(key))!='0':raise ValueError('Storage receipt missing or unexpectedly active')
    after=restrictions(link)
    if after!=before:raise ValueError('Refusal counters changed during read-only health collection')
    return {'health_receipt_passed':True,'version':version,'view':view,'restrictions':after,
            'rf_interoperability_verified':False,'flash_write_readback_verified':False}

def open_link(port):
    if not port or '://' in port or port.startswith('udp:'):
        raise ValueError('An explicit local serial port is required')
    if hashlib.sha256(DEBUG.read_bytes()).hexdigest()!=DEBUG_SHA:
        raise ValueError('Pinned debug protocol module differs')
    spec=importlib.util.spec_from_file_location('firstboot_debug',DEBUG)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    import serial
    class ReadLink(module.DebugLink):
        def __init__(self):
            self.port=port;self.timeout=1;self.seq=0;self.packed_shots=None;self.buf=bytearray()
            self.s=serial.Serial(port=None,baudrate=115200,timeout=0.02,write_timeout=3)
            self.s.rts=self.s.dtr=False;self.s.port=port;self.s.open()
        def request(self,cmd,payload=b'',**kw):
            if cmd not in (1,2,9,0x22):raise ValueError('Only health-query commands are allowed')
            return super().request(cmd,payload,**kw)
    return ReadLink()

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port',required=True);ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args()
    if args.out.exists():raise FileExistsError('Preserving existing receipt')
    report={'health_receipt_passed':False}
    try:
        with open_link(args.port) as link:report=collect(link)
    except Exception as exc:
        report['error']=f'{type(exc).__name__}: {exc}';raise
    finally:
        args.out.parent.mkdir(parents=True,exist_ok=True)
        with args.out.open('x') as f:json.dump(report,f,indent=2);f.write('\n')
    print(json.dumps(report,indent=2))
if __name__=='__main__':main()
