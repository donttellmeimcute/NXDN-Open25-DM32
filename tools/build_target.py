#!/usr/bin/env python3
"""Build and resolve the CK803S software components; never create a radio image."""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / 'toolchain/bin'


def run(*args, **kw):
    return subprocess.run([str(a) for a in args], cwd=ROOT, check=True, **kw)


def main():
    run('python3', ROOT / 'experiments/codec/build.py', '--target')
    run('sh', ROOT / 'tests/protocol/build_target.sh')
    runtime = ROOT / 'build/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    flags = ['-mcpu=ck803s', '-mlittle-endian', '-mno-high-registers', '-msoft-float', '-Os', '-ffreestanding',
             '-fno-builtin', '-fno-tree-loop-distribute-patterns', '-fstack-usage',
             '-std=c11', '-Wall', '-Wextra', '-Werror']
    run(BIN / 'csky-elf-gcc', *flags, '-c', ROOT / 'src/runtime/memory.c', '-o', runtime / 'memory-ck803s.o')
    out = ROOT / 'artifacts/p25-codec-ck803s.relocatable.o'
    out.parent.mkdir(exist_ok=True)
    libgcc = subprocess.check_output([str(BIN / 'csky-elf-gcc'), '-print-libgcc-file-name'], text=True).strip()
    run(BIN / 'csky-elf-ld', '-r', ROOT / 'experiments/codec/out/ck803s/codec-all.o',
        ROOT / 'build/p25-ck803s/p25.o', runtime / 'memory-ck803s.o', libgcc, '-o', out)
    unresolved = subprocess.check_output([str(BIN / 'csky-elf-nm'), '-u', str(out)], text=True)
    if unresolved.strip():
        raise SystemExit('Target software has unresolved symbols:\n' + unresolved)
    result = {'kind': 'relocatable software components; not executable radio firmware',
              'hardware_verified': False, 'sha256': hashlib.sha256(out.read_bytes()).hexdigest(),
              'size': subprocess.check_output([str(BIN / 'csky-elf-size'), str(out)], text=True),
              'unresolved_symbols': [],
              'limitations': ['No RF/modem/audio/interrupt/PTT driver integration',
                              'No verified free stock memory placement',
                              'No target execution/performance measurements',
                              'No flash layout/erase-boundary validation']}
    (ROOT / 'artifacts/target-build.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    run('python3', ROOT / 'tools/build_probe.py')


if __name__ == '__main__':
    main()
