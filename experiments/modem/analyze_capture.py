#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline native-RX analysis; never opens a radio or fills missing air bits.

Build the local CAI library with:
cc -shared -fPIC -O2 -Isrc/p25 src/p25/p25.c -o build/receive-analysis/libp25.dylib
Session/sequence gaps split streams. Concatenation within a segment is a
hypothesis tested by BCH NID and complete-frame FEC, not proof of continuity.
Raw voice and decoded subscriber identifiers are not included in the report.
"""
import argparse
import ctypes as C
import json
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from stream_probe import air_from_rx_ram, apply_mapping, unpack, MAPPINGS

class NID(C.Structure):
    _fields_=[('nac',C.c_uint16),('duid',C.c_uint8),('corrected_bits',C.c_uint8)]

class Decoded(C.Structure):
    _fields_=[('nid',NID),('metadata_valid',C.c_uint8),('clear_voice',C.c_uint8),
        ('manufacturer_id',C.c_uint8),('service_options',C.c_uint8),
        ('link_control_format',C.c_uint8),('algorithm_id',C.c_uint8),
        ('key_id',C.c_uint16),('talkgroup',C.c_uint16),('source_id',C.c_uint32),
        ('message_indicator',C.c_uint8*9),('low_speed_data',C.c_uint8*2),
        ('low_speed_valid',C.c_uint8*2),('metadata_corrected_bits',C.c_uint16),
        ('metadata_corrected_symbols',C.c_uint8),('voice_corrected_bits',C.c_uint8*9),
        ('voice_u0_corrected_bits',C.c_uint8*9),('imbe',(C.c_uint16*8)*9)]

def library(path):
    lib=C.CDLL(str(path.resolve()))
    lib.p25_nid_decode.argtypes=[C.c_uint64,C.POINTER(NID)]
    lib.p25_frame_bytes.argtypes=[C.c_uint8];lib.p25_frame_bytes.restype=C.c_size_t
    lib.p25_decode_frame.argtypes=[C.c_void_p,C.c_size_t,C.POINTER(Decoded)]
    return lib

def analyze(records,lib):
    segments=[]
    for r in records:
        raw=bytes.fromhex(r['raw_hex'])
        if len(raw)!=36:raise ValueError('Expected exactly 36 bytes per native record')
        if not segments or r['session']!=segments[-1][-1]['session'] or r['sequence']!=((segments[-1][-1]['sequence']+1)&0xffffffff):
            segments.append([])
        segments[-1].append(r)
    out={'records':len(records),'segments':[],'concatenation_is_hypothesis':True}
    sync=f'{0x5575f5ff77ff:048b}'
    for rows in segments:
        gaps=[(b['tick']-a['tick'])&0xffffffff for a,b in zip(rows,rows[1:])]
        result={'session':rows[0]['session'],'records':len(rows),
                'sequence_first':rows[0]['sequence'],'sequence_last':rows[-1]['sequence'],
                'gap_min_us':min(gaps) if gaps else None,'gap_max_us':max(gaps) if gaps else None,
                'candidates':[]}
        out['segments'].append(result)
        for layout in ('raw','fig70'):
            chunks=[bytes.fromhex(r['raw_hex']) for r in rows]
            if layout=='fig70':
                try:chunks=[air_from_rx_ram(raw,int(r['rx_type_info'],0)&3) for raw,r in zip(chunks,rows)]
                except ValueError as exc:
                    result['fig70_error']=str(exc);continue
            for mapping in MAPPINGS:
                bits=unpack(apply_mapping(b''.join(chunks),mapping))
                for start in range(max(0,len(bits)-113)):
                    errors=sum(a!=b for a,b in zip(bits[start:start+48],sync))
                    if errors>2:continue
                    n=NID()
                    # Status dibit follows every 70 data bits, measured from sync.
                    nid=''.join(bits[start+j] for j in range(48,114) if j%72 not in (70,71))
                    if lib.p25_nid_decode(int(nid,2),C.byref(n))!=0:continue
                    wanted=lib.p25_frame_bytes(n.duid)*8
                    candidate={'layout':layout,'mapping':mapping,'bit_offset':start,
                        'sync_errors':errors,'nac':f'0x{n.nac:03x}','duid':n.duid,
                        'nid_corrections':n.corrected_bits,'frame_bits':wanted,
                        'available_bits':len(bits)-start,'complete_frame_available':len(bits)-start>=wanted}
                    if candidate['complete_frame_available']:
                        frame=int(bits[start:start+wanted],2).to_bytes(wanted//8,'big')
                        decoded=Decoded()
                        candidate['frame_decode_ok']=lib.p25_decode_frame(frame,len(frame),C.byref(decoded))==0
                        candidate['metadata_valid']=bool(decoded.metadata_valid)
                        candidate['clear_voice']=bool(decoded.clear_voice)
                        candidate['metadata_corrected_bits']=decoded.metadata_corrected_bits
                        candidate['metadata_corrected_symbols']=decoded.metadata_corrected_symbols
                        if n.duid in (5,10):
                            candidate['voice_corrected_bits']=list(decoded.voice_corrected_bits)
                            candidate['voice_u0_corrected_bits']=list(decoded.voice_u0_corrected_bits)
                    result['candidates'].append(candidate)
    return out

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--lib',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('captures',type=Path,nargs='+')
    args=ap.parse_args()
    records=[json.loads(line) for path in args.captures for line in path.read_text().splitlines() if line.strip()]
    report=analyze(records,library(args.lib))
    with args.out.open('x') as f:json.dump(report,f,indent=2);f.write('\n')
    print(json.dumps(report,indent=2))
if __name__=='__main__':main()
