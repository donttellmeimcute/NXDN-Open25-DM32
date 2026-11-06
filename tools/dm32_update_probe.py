#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Identify the DM32 update loader. Three identification requests; no erase/write.

Enter update mode first (PTT+SK1 at power-on, green LED). This tool does not
install an image or reset the radio. Exit update mode with a normal power cycle.
"""
import argparse
import json
from pathlib import Path
import time


class ProbePort:
    def __init__(self,port): self.raw=port; self.stage=0; self.attempts=0; self.sent=[]
    def write(self,data):
        data=bytes(data)
        if self.stage==0 and data==b'\x52' and self.attempts<3:
            self.attempts+=1
        elif self.stage==0 and self.attempts and data==bytes.fromhex('4d00000001'):
            self.stage=1
        elif self.stage==1 and data==b'\x06': self.stage=2
        else: raise ValueError('Only the three identification requests are permitted')
        n=self.raw.write(data)
        if n is not None and n!=len(data): raise OSError('Short serial write')
        self.sent.append(data.hex())
        flush=getattr(self.raw,'flush',None)
        if flush: flush()
    def read(self,n): return self.raw.read(n)


def exact(port,n,timeout):
    result=bytearray();end=time.monotonic()+timeout
    while len(result)<n and time.monotonic()<end: result.extend(port.read(n-len(result)))
    if len(result)!=n: raise TimeoutError('Update loader response incomplete')
    return bytes(result)


def identify(raw,timeout=2):
    port=ProbePort(raw)
    for attempt in range(3):
        port.write(b'\x52')
        try: reply=exact(port,1,timeout)
        except TimeoutError:
            if attempt==2: raise
            continue
        if reply!=b'\x06': raise ValueError('Not the expected update-loader acknowledgment')
        break
    port.write(bytes.fromhex('4d00000001'))
    step2=exact(port,3,timeout)
    if step2!=bytes.fromhex('4d0109'): raise ValueError('Unexpected update-loader protocol identification')
    port.write(b'\x06');model=exact(port,9,timeout)
    if model!=b'BFUV32-V2': raise ValueError('Update loader reports a different target')
    return {'loader_id':model.decode('ascii'),'step2':step2.hex(),'identification_passed':True,
            'requests_hex':port.sent,'erase_or_program_commands_sent':0,'stopped_before_step':4,
            'restore_write_tested':False}


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port',required=True);ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args()
    if args.out.exists(): raise FileExistsError(args.out)
    import serial
    raw=serial.Serial(port=None,baudrate=115200,timeout=0.05,write_timeout=3)
    raw.rts=raw.dtr=False;raw.port=args.port
    report={'identification_passed':False,'erase_or_program_commands_sent':0}
    try:
        raw.open();raw.reset_input_buffer();report=identify(raw)
    except Exception as exc:
        report['error']=f'{type(exc).__name__}: {exc}';raise
    finally:
        raw.close();args.out.parent.mkdir(parents=True,exist_ok=True)
        with args.out.open('x') as out: json.dump(report,out,indent=2);out.write('\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__': main()
