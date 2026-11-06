#!/usr/bin/env python3
"""Hash-pinned extraction and C-SKY ABIv2 ELF generation, for static analysis only."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def analyze(image, out, target):
    meta = json.loads(target.read_text())
    data = image.read_bytes()
    if sha(data) != meta['image_sha256'] or len(data) != meta['image_size']:
        raise ValueError('Input is not the pinned DM32.01.01.049 image')
    if data[:9] != b'BFUV32-V2' or data[253:256] != b'END':
        raise ValueError('Unexpected container header')
    # Directly checked startup literal pool and copy loops in this exact image.
    base = 0x0300c000
    literal = lambda offset: struct.unpack_from('<I', data, 0x100 + offset)[0]
    expected = {0xb4: 0x50000, 0xb8: 0x10000, 0xc4: 0x18000800,
                0xc8: 0x18004bfc, 0xcc: 0x03071f80, 0xd0: 0x10000,
                0xd4: 0x2f098, 0xd8: 0x030bfba0, 0xdc: 0x2f098,
                0xe0: 0x4a7b0}
    for off, value in expected.items():
        if literal(off) != value:
            raise ValueError(f'Unexpected startup literal at payload {off:#x}')
    out.mkdir(parents=True, exist_ok=True)
    segments = [('flash', base, 0x100, len(data) - 0x100),
                ('iram', literal(0xd0), literal(0xd8) - base + 0x100,
                 literal(0xd4) - literal(0xd0)),
                ('sahb', literal(0xc4), literal(0xcc) - base + 0x100,
                 literal(0xc8) - literal(0xc4))]
    objcopy = ROOT / 'toolchain/bin/csky-elf-objcopy'
    objdump = ROOT / 'toolchain/bin/csky-elf-objdump'
    records = []
    for name, address, offset, size in segments:
        raw = data[offset:offset + size]
        if len(raw) != size:
            raise ValueError('Segment exceeds image')
        binary = out / f'{name}.analysis.bin'
        elf = out / f'{name}.analysis.elf'
        binary.write_bytes(raw)
        subprocess.run([str(objcopy), '-I', 'binary', '-O', 'elf32-csky-little',
                        '-B', 'csky:ck803', '--change-section-address', f'.data={address:#x}',
                        str(binary), str(elf)], check=True)
        # Raw binary has no architecture flags. -m csky:ck803 alone does NOT set
        # ABIv2 for binutils 2.45.1 and silently splits 32-bit instructions.
        # Set only the analysis wrapper's ELF flags (never firmware bytes).
        wrapped = bytearray(elf.read_bytes())
        struct.pack_into('<I', wrapped, 36, 0x20000009)
        elf.write_bytes(wrapped)
        with (out / f'{name}.asm').open('w') as dest:
            subprocess.run([str(objdump), '-D', str(elf)], stdout=dest, check=True)
        records.append({'segment': name, 'image_sha256': sha(data),
                        'file_offset': offset, 'runtime_address': address,
                        'size': size, 'sha256': sha(raw),
                        'confidence': 'direct startup loop/literal observation'})
    result = {'segments': records,
              'zeroed': {'start': literal(0xdc), 'end_exclusive': literal(0xe0)},
              'initial_stack': literal(0xb4),
              'vector_base': literal(0xb8),
              'application_entry_candidate': base,
              'next_stage_call': 0x2e158,
              'warning': 'Static analysis only; not a full-chip image or flash candidate.'}
    (out / 'memory-map.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image', type=Path, default=ROOT / 'research/originals/DM32.01.01.049.bin')
    ap.add_argument('--target', type=Path, default=ROOT / 'targets/dm32-01-01-049-public.json')
    ap.add_argument('--out', type=Path, default=ROOT / 'artifacts/map-049')
    args = ap.parse_args()
    analyze(args.image, args.out, args.target)
