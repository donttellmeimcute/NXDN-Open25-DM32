#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read a local built ELF/map; report link-time RAM/flash and largest globals."""
import argparse
import json
from pathlib import Path
import re
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def budget(elf):
    text=elf.with_suffix('.map').read_text()
    m=re.search(r'^RAM\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)',text,re.M)
    if not m:raise ValueError('Missing RAM region')
    origin,capacity=(int(s,16) for s in m.groups())
    sections=subprocess.check_output([str(ROOT/'toolchain/bin/csky-elf-objdump'),'-h',str(elf)],text=True)
    ends=[]
    for line in sections.splitlines():
        cols=line.split()
        if len(cols)>=6 and cols[0].isdigit():
            size,addr=int(cols[2],16),int(cols[3],16)
            if origin<=addr<origin+capacity:ends.append(addr+size)
    if not ends or max(ends)>origin+capacity:raise ValueError('Invalid RAM section bounds')
    symbols=subprocess.check_output([str(ROOT/'toolchain/bin/csky-elf-nm'),'-S','--size-sort',str(elf)],text=True)
    ram=[]
    for line in symbols.splitlines():
        cols=line.split()
        if len(cols)==4 and cols[2].lower() in ('b','d') and origin<=int(cols[0],16)<origin+capacity:
            ram.append({'symbol':cols[3],'bytes':int(cols[1],16)})
    ram.sort(key=lambda s:s['bytes'],reverse=True)
    return {'flash_bytes':elf.with_suffix('.bin').stat().st_size,'main_ram_bytes':max(ends)-origin,
            'main_ram_capacity_bytes':capacity,'main_ram_unallocated_bytes':origin+capacity-max(ends),
            'largest_ram_symbols':ram[:20]}

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--elf',type=Path,required=True);ap.add_argument('--baseline',type=Path)
    ap.add_argument('--out',type=Path,required=True);a=ap.parse_args()
    if a.out.exists():raise FileExistsError(a.out)
    r={'candidate':budget(a.elf),'hardware_cpu_usage_measured':False}
    if a.baseline:
        r['baseline']=budget(a.baseline)
        r['saved_bytes']={k:r['baseline'][k]-r['candidate'][k] for k in ('flash_bytes','main_ram_bytes')}
    a.out.parent.mkdir(parents=True,exist_ok=True)
    with a.out.open('x') as f:json.dump(r,f,indent=2);f.write('\n')
    print(json.dumps({k:v for k,v in r.items() if k=='saved_bytes'},indent=2))
if __name__=='__main__':main()
