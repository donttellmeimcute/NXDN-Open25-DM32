#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode a verified, consecutive clear P25 LDU run from one private RX file.

Offline only. Uses Figure 70 identity mapping, rejects frame gaps, unsupported
metadata, encryption and corrected voice words for this strict bench export.
The file is decoded on the host; this does not demonstrate on-radio playback.
"""
import argparse
import ctypes as C
import json
from pathlib import Path
import struct
import wave
from analyze_capture import analyze, library, Decoded
from stream_probe import air_from_rx_ram, unpack

def decode(records,cai,codec):
    report=analyze(records,cai)
    if len(report['segments'])!=1:raise ValueError('One consecutive session required')
    candidates=[c for c in report['segments'][0]['candidates'] if c['layout']=='fig70' and
        c['mapping']=='identity' and c['duid'] in (5,10) and c['complete_frame_available']]
    if not candidates or candidates[0]['duid']!=5:raise ValueError('No complete LDU1 for clear late entry')
    air=unpack(b''.join(air_from_rx_ram(bytes.fromhex(r['raw_hex']),int(r['rx_type_info'],0)&3) for r in records))
    codec.dm32_imbe_init.argtypes=[C.c_void_p]
    codec.dm32_imbe_decode.argtypes=[C.c_void_p,C.c_void_p,C.c_void_p]
    context=(C.c_uint64*1024)();codec.dm32_imbe_init(context)
    samples=[];previous=None
    for c in candidates:
        if previous is not None and (c['bit_offset']!=previous['bit_offset']+1728 or c['duid']==previous['duid']):
            raise ValueError('Voice frame discontinuity; refusing to join audio across a gap')
        if not c.get('frame_decode_ok') or not c.get('metadata_valid') or not c.get('clear_voice'):
            raise ValueError('Clear standard voice metadata required throughout the run')
        if any(c['voice_corrected_bits']) or any(c['voice_u0_corrected_bits']):
            raise ValueError('This strict bench export requires zero corrected voice bits')
        frame=int(air[c['bit_offset']:c['bit_offset']+1728],2).to_bytes(216,'big');decoded=Decoded()
        if cai.p25_decode_frame(frame,len(frame),C.byref(decoded))!=0:raise ValueError('Frame decode failed')
        for u in decoded.imbe:
            pcm=(C.c_int16*160)()
            if codec.dm32_imbe_decode(context,u,pcm)!=1:raise ValueError('IMBE arithmetic fault')
            samples.extend(pcm)
        previous=c
    return samples,{'ldus':len(candidates),'imbe_frames':len(candidates)*9,'sample_rate_hz':8000,
        'samples':len(samples),'seconds':len(samples)/8000,'peak':max(abs(s) for s in samples),
        'nonzero_samples':sum(s!=0 for s in samples),'all_voice_fec_corrections_zero':True,
        'host_offline_decode':True,'on_radio_playback_demonstrated':False}

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture',type=Path);ap.add_argument('--cai-lib',type=Path,required=True)
    ap.add_argument('--codec-lib',type=Path,required=True);ap.add_argument('--wav',type=Path,required=True)
    ap.add_argument('--report',type=Path,required=True);args=ap.parse_args()
    if args.wav.exists() or args.report.exists():raise FileExistsError('Preserving previous output')
    records=[json.loads(l) for l in args.capture.read_text().splitlines() if l.strip()]
    samples,report=decode(records,library(args.cai_lib),C.CDLL(str(args.codec_lib.resolve())))
    with args.wav.open('xb') as out:
        with wave.open(out,'wb') as wav:
            wav.setnchannels(1);wav.setsampwidth(2);wav.setframerate(8000)
            wav.writeframes(struct.pack('<'+'h'*len(samples),*samples))
    with args.report.open('x') as out:json.dump(report,out,indent=2);out.write('\n')
    print(json.dumps(report,indent=2))
if __name__=='__main__':main()
