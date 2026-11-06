#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""RAM-only updater-entry rehearsal: no flash-array reads or flash writes."""
import argparse
import json
from pathlib import Path
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from tools import dm32_readonly_backup as backup
from tools import firstboot_check as check
from tools import warm_update
def frame(address,value):
    p=address.to_bytes(4,'big')+value.to_bytes(4,'little')
    return bytes([2,3,0,len(p),sum(p)&255,3])+p
class ResetOnlyPort(backup.ReadOnlyPort):
    def __init__(self,raw,m):
        super().__init__(raw,m);self.monitor=True;self.armed=False;self.index=0
        self.plan=[frame(a,v) for a,v in ((m.CRASH_SOFT_MARK,m.CRASH_BOOT_SOFT),(m.WDT_LOCK,m.WDT_KEY),
            (m.WDT_EN,0),(m.WDT_LOCK,0),(m.QUAD_ENABLE,0),(m.LCSFC_BAUDR,4),(m.SYS_SOFT_RSTN,0))]
    def arm(self):self.armed=True
    def validate(self,data):
        if self.armed and self.index<len(self.plan) and data==self.plan[self.index]:
            self.index+=1;return 'fixed_restart_register'
        # Zero flash capacities deliberately refuse all array reads; the
        # inherited transport also refuses flash writes and arbitrary POKE.
        return super().validate(data)
def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--port',required=True);ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args()
    if a.out.exists():raise FileExistsError(a.out)
    r={'flash_reads_requested':False,'flash_writes_requested':False,'rf_transmit_requested':False,'passed':False}
    raw=None
    try:
        import serial
        m=backup.load_upstream();raw=serial.Serial(port=None,baudrate=115200,timeout=0.02,write_timeout=3)
        raw.rts=raw.dtr=False;raw.port=a.port;raw.open()
        r['entry']=warm_update.enter(a.port,raw)
        port=ResetOnlyPort(raw,m);radio=m.Radio(port,log=lambda s:print(s,flush=True),retries=0)
        raw.reset_input_buffer();radio.hello(retries=3);radio.configure('dm32')
        if not radio.configured or radio.v6['board']!=1 or radio.v6['flags']!=0:raise ValueError('RAM-helper board echo differs')
        for mem,want in (('progmem','c84014'),('q128','ef4018')):
            if radio.jedec(mem).hex()!=want:raise ValueError('Attached chip ID differs')
        r['helper_verified']=True;port.arm();radio.reset_chip();r['reset_frames']=port.index
        if port.index!=7:raise ValueError('Incomplete fixed restart sequence')
        raw.close();time.sleep(3)
        with check.open_link(a.port) as link:r['health']=check.collect(link)
        r['operations']=port.counts;r['passed']=True
    except BaseException as exc:r['error']=f'{type(exc).__name__}: {exc}';raise
    finally:
        if raw is not None and raw.is_open:raw.close()
        a.out.parent.mkdir(parents=True,exist_ok=True)
        with a.out.open('x') as f:json.dump(r,f,indent=2);f.write('\n')
    print(json.dumps({'passed':r['passed'],'flash_reads_requested':False,'flash_writes_requested':False,'reset_frames':r['reset_frames']},indent=2))
if __name__=='__main__':main()
