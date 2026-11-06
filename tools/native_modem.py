#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded native-PHY bench interface; hardware-unverified. Never flashes.

Explicit port, current idle FM channel and frequency match required. Transmit
requires --allow-rf; a 1 ms device timer enforces its lease and hard duration.
Software acknowledgments do not establish RF continuity or P25 compliance.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import time
import hashlib
import subprocess

ROOT=Path(__file__).resolve().parents[1]
CMD=0x21
FIELDS="version state reason session owned mode frequency_hz blocks duration_ms lease_ms rx_count tx_count rx_seen rx_saved rx_dropped rx_read tx_loaded tx_sent phy_irqs tx_slot_irqs rx_slot_irqs rf_irqs abnormal_irqs duplicate_irqs ignored_irqs pa_keys quiets faults rejected max_gap_us last_gap_us restore_failures started_us boundary_us p25_sync key_requested keyed".split()
STATES=("idle","armed","receive","transmit","draining","stopped","fault")
REASONS=("ok","user_stop","rx_limit","tx_complete","rx_duration","lease_expired","deadline","stalled","late_boundary","early_boundary","underrun","io","rx_overflow","abnormal_slot","denied","interrupted","restore_failed","unexpected_rf")
SNAPSHOT_REGS='work_mode layer2_control slot_on txrx_control sys_mask ms_sync_h ms_sync_l bs_sync_h bs_sync_l tdma1_h tdma1_l tdma2_h tdma2_l rc_h rc_l rf_mode rf_control soft_reset recv_info_count rx_type sys_list layer2_status sig_center rx_if_freq'.split()

def request(link, op, body=b""):
    status,data=link.request(CMD,bytes([op])+body,busy_wait=0)
    if status: raise RuntimeError(f"native operation {op} refused: status {status}")
    return data

def status(link):
    data=request(link,0)
    if len(data)!=len(FIELDS)*4: raise RuntimeError("native status length mismatch")
    r=dict(zip(FIELDS,struct.unpack("<"+"I"*len(FIELDS),data)))
    if r["version"]!=1 or r["state"]>=len(STATES) or r["reason"]>=len(REASONS):
        raise RuntimeError("unsupported native protocol/status")
    return r

def snapshot(link):
    data=request(link,8)
    if len(data)!=224:raise RuntimeError('Native snapshot length mismatch')
    words=struct.unpack('<56I',data)
    if words[0]!=1:raise RuntimeError('Native snapshot version mismatch')
    return {'version':words[0],'prepared':words[1],'saved_valid':words[2],
            'last_event':words[3],'last_event_us':words[4],'last_sys_flags':words[5],
            'last_rx_type':words[6],'last_sys_us':words[7],
            'current':dict(zip(SNAPSHOT_REGS,words[8:32])),
            'last_event_registers':dict(zip(SNAPSHOT_REGS,words[32:56]))}

def validate(mode,frequency,blocks,duration,lease,sync,allow_rf,tx_preload=False):
    if mode not in (1,2) or not 1<=frequency<=0xffffffff or not 1<=blocks<=60:
        raise ValueError("invalid mode, frequency or block count (1..60)")
    if tx_preload:
        if mode!=2 or frequency!=145500000 or blocks>28 or duration!=1000 or lease!=1000 or sync!=1:
            raise ValueError("Preloaded burst requires the bounded 145.500 MHz TX profile")
    elif not 1<=duration<=2000 or not 100<=lease<=500 or sync not in (0,1):
        raise ValueError("duration must be 1..2000 ms, lease 100..500 ms")
    if mode==2 and not allow_rf: raise ValueError("transmit requires --allow-rf")
    if mode==2 and duration<blocks*30+150: raise ValueError("duration too short for TX blocks and startup/drain allowance")

def payloads(pattern,count,air=None):
    if air is not None:
        if not air or len(air)>60*36: raise ValueError("air stream must contain 1..2160 bytes")
        pad=(-len(air))%36
        stream=air+bytes(pad)
        return [stream[i:i+36] for i in range(0,len(stream),36)],pad
    if not 1<=count<=60: raise ValueError("block count must be 1..60")
    values={"plus1":0x00,"plus3":0x55,"minus1":0xaa,"minus3":0xff,"p25-cal":0x5f}
    if pattern in values: return [bytes([values[pattern]])*36 for _ in range(count)],0
    if pattern!="counter": raise ValueError("unknown pattern")
    seed=0x700025
    out=[]
    for i in range(count):
        b=bytearray(struct.pack("<I",i))
        while len(b)<36:
            seed^=(seed<<13)&0xffffffff;seed^=seed>>17;seed^=(seed<<5)&0xffffffff
            b.append(seed&255)
        out.append(bytes(b))
    return out,0

def run_session(link,*,mode,frequency,blocks=16,duration=2000,lease=250,sync=1,allow_rf=False,
                data=(),clock=time.monotonic,sleep=time.sleep,tx_preload=False):
    validate(mode,frequency,blocks,duration,lease,sync,allow_rf,tx_preload)
    if mode==2 and (len(data)!=blocks or any(len(b)!=36 for b in data)):
        raise ValueError("TX requires exactly the declared complete 36-byte blocks")
    result={"hardware_verified":False,"rf_interoperability_verified":False,"capture":[],"cleanup_confirmed":False}
    sid=None
    try:
        arm=bytes([mode,sync])+struct.pack("<HHHI",blocks,duration,lease,frequency)+struct.pack("<I",0x3532504e if allow_rf else 0)
        reply=request(link,1,arm)
        if len(reply)!=4: raise RuntimeError("ARM reply length mismatch")
        sid,=struct.unpack("<I",reply)
        if not sid: raise RuntimeError("zero session ID")
        result["session"]=sid
        sent=0
        for i in range(len(data) if tx_preload else min(8,len(data))):
            request(link,2,struct.pack("<IH",sid,i)+data[i]);sent+=1
        request(link,3,struct.pack("<I",sid))
        if tx_preload:
            result["missed_tx_replies"]=0
        deadline=clock()+duration/1000+2
        while True:
            if clock()>=deadline: raise TimeoutError("native test did not finish within bounded host deadline")
            try:
                r=status(link)
            except TimeoutError:
                if not tx_preload: raise
                result["missed_tx_replies"]+=1
                # A fully loaded burst has its own hard deadline. A delayed
                # status reply cannot interrupt the sample or start another.
                sleep(0.025)
                continue
            if r["session"]!=sid: raise RuntimeError("device session changed")
            result["status"]=r
            if r["state"] in (2,3,4) and not tx_preload:
                try: request(link,4,struct.pack("<I",sid))
                except Exception:
                    # The device timer may finish a bounded RX window after
                    # STATUS but before PING. Confirm the same terminal session;
                    # preserve real faults and propagate every other failure.
                    fresh=status(link)
                    if fresh["session"]!=sid or fresh["state"] not in (5,6):raise
                    r=fresh;result["status"]=r
            for _ in range(r["rx_count"]):
                raw=request(link,6,struct.pack("<I",sid))
                if len(raw)!=52: raise RuntimeError("RX record length mismatch")
                seq,stamp,flags,info=struct.unpack("<IIII",raw[:16])
                previous=result["capture"][-1]["sequence"] if result["capture"] else None
                if previous is not None and seq!=(previous+1)&0xffffffff: raise RuntimeError("capture sequence gap")
                result["capture"].append({"session":sid,"sequence":seq,"tick":stamp,
                    "sys_flags":hex(flags),"rx_type_info":hex(info),"raw_hex":raw[16:].hex()})
            if r["state"] in (5,6): break
            if mode==2 and r["state"]==3 and not tx_preload:
                for _ in range(min(8-r["tx_count"],len(data)-sent)):
                    request(link,2,struct.pack("<IH",sid,sent)+data[sent]);sent+=1
            if clock()>=deadline: raise TimeoutError("native test did not finish within bounded host deadline")
            sleep(0.025)
    except (Exception,KeyboardInterrupt) as e:
        result["error"]=f"{type(e).__name__}: {e}"
    finally:
        if sid is not None:
            try:
                request(link,5,struct.pack("<I",sid))
                end=clock()+0.3
                while True:
                    final=status(link)
                    result["status"]=final
                    if final["session"]!=sid: raise RuntimeError("session changed during cleanup")
                    if not final["owned"] and final["state"] in (5,6):
                        result["cleanup_confirmed"]=True;break
                    if clock()>=end: raise TimeoutError("device has not confirmed radio ownership restored")
                    sleep(0.01)
            except Exception as e: result["cleanup_error"]=f"{type(e).__name__}: {e}"
    return result

def open_link(port):
    upstream=ROOT/"research/upstream/radiofoundry"
    pin=subprocess.check_output(["git","-C",str(upstream),"rev-parse","HEAD"],text=True).strip()
    if pin!="dfbaf20c400c73e2ccf6edb80af7d1d255bee418":
        raise RuntimeError("RadioFoundry protocol pin mismatch")
    changes=subprocess.check_output(["git","-C",str(upstream),"status","--porcelain","--","tools/rfoundry_debug.py"],text=True)
    if changes: raise RuntimeError("Preserving changed RadioFoundry protocol source; refusing import")
    spec=importlib.util.spec_from_file_location("rf_debug",upstream/"tools/rfoundry_debug.py")
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module.DebugLink(port=port,timeout=0.15)

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port",required=True)
    sub=ap.add_subparsers(dest="command",required=True)
    sub.add_parser("status");sub.add_parser("stop");sub.add_parser("reset")
    run=sub.add_parser("run")
    run.add_argument("--mode",choices=("rx","tx"),required=True)
    run.add_argument("--frequency",type=int,required=True,help="must match the current idle FM channel, in Hz")
    run.add_argument("--blocks",type=int,default=16)
    run.add_argument("--duration-ms",type=int,default=2000)
    run.add_argument("--lease-ms",type=int,default=250)
    run.add_argument("--sync",choices=("p25","inherited"),default="p25")
    run.add_argument("--pattern",choices=("counter","plus1","plus3","minus1","minus3","p25-cal"),default="counter")
    run.add_argument("--air",type=Path,help="packed CAI reference file, padded to a whole hardware block")
    run.add_argument("--allow-rf",action="store_true")
    run.add_argument("--output",type=Path,required=True)
    args=ap.parse_args()
    data=[];pad=0
    if args.command=="run":
        try:
            mode=1 if args.mode=="rx" else 2
            if mode==1 and args.air: raise ValueError("--air is for TX only")
            if mode==2:
                data,pad=payloads(args.pattern,args.blocks,args.air.read_bytes() if args.air else None)
                args.blocks=len(data)
            validate(mode,args.frequency,args.blocks,args.duration_ms,args.lease_ms,args.sync=="p25",args.allow_rf)
        except ValueError as e: ap.error(str(e))
    with open_link(args.port) as link:
        if args.command!="run":
            r=status(link)
            if args.command in ("stop","reset"): request(link,5 if args.command=="stop" else 7,struct.pack("<I",r["session"]));r=status(link)
            print(json.dumps(r,indent=2));return
        r=run_session(link,mode=mode,frequency=args.frequency,blocks=args.blocks,duration=args.duration_ms,
              lease=args.lease_ms,sync=int(args.sync=="p25"),allow_rf=args.allow_rf,data=data)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    capture=r.pop("capture")
    args.output.with_suffix(".jsonl").write_text("".join(json.dumps(row)+"\n" for row in capture))
    if data:
        expected=b"".join(data);args.output.with_suffix(".air").write_bytes(expected)
        r.update({"expected_sha256":hashlib.sha256(expected).hexdigest(),"non_cai_tail_bytes":pad})
    if "status" in r:
        r["state_name"]=STATES[r["status"]["state"]];r["reason_name"]=REASONS[r["status"]["reason"]]
    r["captured_records"]=len(capture)
    args.output.with_suffix(".json").write_text(json.dumps(r,indent=2)+"\n")
    print(json.dumps(r,indent=2))
    if r.get("error") or not r["cleanup_confirmed"] or r.get("status",{}).get("state")==6: raise SystemExit(2)
if __name__=="__main__": main()
