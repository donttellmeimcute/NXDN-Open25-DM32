#!/usr/bin/env python3
"""Build an independently linked C-SKY RAM diagnostic, not a firmware update."""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'artifacts/startup-probe'
BIN = ROOT / 'toolchain/bin'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    flags = ['-mcpu=ck803s', '-mlittle-endian', '-msoft-float', '-Os', '-ffreestanding',
             '-fno-builtin', '-fno-stack-protector', '-fno-pic', '-fstack-usage',
             '-Wall', '-Wextra', '-Werror']
    for name, language in [('probe.c', ['-std=c11']), ('start.S', [])]:
        subprocess.run([str(BIN / 'csky-elf-gcc'), *flags, *language, '-c',
                        str(ROOT / 'experiments/startup-probe' / name), '-o',
                        str(OUT / (name.split('.')[0] + '.o'))], check=True)
    subprocess.run([str(BIN / 'csky-elf-ld'), '-T', str(ROOT / 'experiments/startup-probe/probe.ld'),
                    '-Map=' + str(OUT / 'probe.map'), str(OUT / 'start.o'), str(OUT / 'probe.o'),
                    '-o', str(OUT / 'probe.elf')], check=True)
    subprocess.run([str(BIN / 'csky-elf-objcopy'), '-O', 'binary', str(OUT / 'probe.elf'),
                    str(OUT / 'probe.ram.bin')], check=True)
    with (OUT / 'probe.asm').open('w') as f:
        subprocess.run([str(BIN / 'csky-elf-objdump'), '-d', str(OUT / 'probe.elf')], stdout=f, check=True)
    image = (OUT / 'probe.ram.bin').read_bytes()
    manifest = {'kind': 'ram-diagnostic-only', 'hardware_verified': False,
                'load_address': 0x10000, 'entry_address': 0x10000,
                'length': len(image), 'sha256': hashlib.sha256(image).hexdigest(),
                'cpu': 'ck803s', 'endianness': 'little', 'abi': 'C-SKY ABIv2',
                'compiler': subprocess.check_output([str(BIN / 'csky-elf-gcc'), '-dumpfullversion'], text=True).strip(),
                'flags': flags, 'flash_writes': False, 'rf_writes': False,
                'expected_output': 'DM32-RAM-PROBE/1\\r\\ncrc16=000031c3\\r\\nbyte0=00000078\\r\\nwork_mode=<8 hex digits>\\r\\nDONE; power-cycle to restore normal boot\\r\\n',
                'precondition': 'Complete verified original backups first; read docs/recovery.md.'}
    (OUT / 'probe.manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
