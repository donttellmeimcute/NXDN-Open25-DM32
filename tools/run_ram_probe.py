#!/usr/bin/env python3
"""Plan or load ONLY the built, hash-checked RAM probe, after verified backups."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import stat
import sys
import time

from dm32_backup import (ACK, BOOT, CAPACITIES, HELPER_SHA256,
                         ORIGINAL_HELPER_SHA256, Reader, packet, sha)

ROOT = Path(__file__).resolve().parents[1]
# Pin is independent of the supplied manifest. Update only after source and
# target disassembly review, not from a caller's input or unverified new build.
REVIEWED_PROBE_SHA256 = 'd3e9f7f0c58a91c701ff8d36d24cf2ef02e946f338169ec61bc753806ed4201a'


def probe_plan(binary, manifest):
    data = binary.read_bytes()
    spec = json.loads(manifest.read_text())
    if (spec['kind'] != 'ram-diagnostic-only' or spec['cpu'] != 'ck803s' or
            spec['load_address'] != 0x10000 or spec['entry_address'] != 0x10000 or
            spec['endianness'] != 'little' or spec['abi'] != 'C-SKY ABIv2' or
            spec['flash_writes'] or spec['rf_writes']):
        raise ValueError('Not the supported standalone RAM diagnostic')
    if (len(data) != spec['length'] or sha(data) != spec['sha256'] or
            sha(data) != REVIEWED_PROBE_SHA256 or not 0 < len(data) <= 0x1000):
        raise ValueError('Probe hash/size mismatch')
    writes = []
    for offset in range(0, len(data), 256):
        block = data[offset:offset + 256].ljust(256, b'\0')
        payload = (0x10000 + offset).to_bytes(4, 'big') + (64).to_bytes(4, 'big') + block
        writes.append(packet(0x18, payload))
    execute = packet(0x1b, (0x10000).to_bytes(4, 'big'))
    return spec, writes, execute


def verify_backup(directory):
    manifest = json.loads((directory / 'backup-manifest.json').read_text())
    if manifest.get('capture_completed') is not True:
        raise ValueError('Backup session is incomplete')
    if (manifest.get('helper_sha256') != HELPER_SHA256 or
            manifest.get('original_helper_sha256') != ORIGINAL_HELPER_SHA256 or
            manifest.get('helper_source_commit') != '4b2185944a85a578ae33626922b763db6256533e'):
        raise ValueError('Backup helper provenance mismatch')
    if manifest.get('installed_firmware') != 'DM32.01.01.049':
        raise ValueError('RAM probe currently restricted to identified DM32.01.01.049; do not cross families')
    captures = manifest['captures']
    if len(captures) != 4:
        raise ValueError('Require two complete program/data passes')
    seen, inodes, hashes = set(), set(), {}
    for capture in captures:
        chip, index, size = capture['chip'], capture['pass'], capture['bytes']
        if chip not in CAPACITIES or size not in CAPACITIES[chip] or index not in (1, 2):
            raise ValueError('Unknown chip, range or capture pass')
        if (chip, index) in seen:
            raise ValueError('Duplicate backup capture')
        seen.add((chip, index))
        name = Path(capture['file'])
        if str(name) != f'{chip}-pass{index}.bin':
            raise ValueError('Require distinct generated backup filenames')
        path = directory / name
        if path.is_symlink():
            raise ValueError('Backup capture cannot be a symlink')
        info = path.stat()
        inode = (info.st_dev, info.st_ino)
        if not stat.S_ISREG(info.st_mode) or inode in inodes:
            raise ValueError('Backup files must be distinct regular files, without aliases')
        inodes.add(inode)
        h = hashlib.sha256()
        with path.open('rb') as stream:
            for block in iter(lambda: stream.read(1 << 20), b''):
                h.update(block)
        if path.stat().st_size != size or h.hexdigest() != capture['sha256']:
            raise ValueError('Backup file size/hash mismatch')
        hashes[(chip, index)] = (size, h.hexdigest())
    if seen != {('program', 1), ('program', 2), ('data', 1), ('data', 2)}:
        raise ValueError('Missing backup capture')
    for chip in ('program', 'data'):
        if hashes[(chip, 1)] != hashes[(chip, 2)]:
            raise ValueError('Backup passes differ; resolve changing regions before the probe')
    return manifest


def execute_probe(transport, writes, execute, timeout=30.0):
    reader = Reader(transport)
    deadline = time.monotonic() + timeout
    window = b''
    while time.monotonic() < deadline:
        window = (window + transport.read(1))[-4:]
        if window == BOOT:
            break
    else:
        raise TimeoutError('No C7000 startup handshake')
    reader.write(b'\x02\x14\x00\x03')
    time.sleep(0.05)
    for block in writes:
        reader.write(block)
        reader.expect_ack()
    reader.write(execute)
    reader.expect_ack()
    output = bytearray()
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline and len(output) < 1024:
        output.extend(transport.read(min(128, 1024 - len(output))))
        if b'DONE; power-cycle to restore normal boot\r\n' in output:
            break
    expected = (rb'DM32-RAM-PROBE/1\r\ncrc16=000031c3\r\nbyte0=00000078\r\n'
                rb'work_mode=[0-9a-f]{8}\r\nDONE; power-cycle to restore normal boot\r\n')
    if not re.fullmatch(expected, bytes(output)):
        raise IOError('Probe did not return complete expected output: ' + bytes(output).hex())
    return bytes(output)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', type=Path, default=ROOT / 'artifacts/startup-probe/probe.ram.bin')
    ap.add_argument('--manifest', type=Path, default=ROOT / 'artifacts/startup-probe/probe.manifest.json')
    ap.add_argument('--port', help='Omit for an offline plan; never auto-selects a port')
    ap.add_argument('--backup', type=Path, help='Completed two-pass backup session directory')
    ap.add_argument('--out', type=Path, help='New file for actual probe result')
    args = ap.parse_args()
    spec, writes, execute = probe_plan(args.binary, args.manifest)
    if args.port is None:
        print(json.dumps({'mode': 'offline-plan', 'sha256': spec['sha256'], 'RAM_blocks': len(writes),
                          'load': '0x10000', 'execute': '0x10000', 'flash_write_commands': 0,
                          'hardware_verified': False}, indent=2))
        return
    if args.backup is None or args.out is None:
        ap.error('Live probe requires --backup and --out')
    if args.out.exists():
        raise FileExistsError(args.out)
    backup = verify_backup(args.backup)
    import serial
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=3)
    port.dtr = False
    port.rts = False
    port.port = args.port
    port.open()
    try:
        print('With the radio powered off and cable seated, power on for the startup handshake.', file=sys.stderr)
        output = execute_probe(port, writes, execute)
    finally:
        port.close()
    result = {'kind': 'RAM startup diagnostic result only', 'unit': backup['identity'],
              'installed_firmware': backup['installed_firmware'], 'probe_sha256': spec['sha256'],
              'serial_output': output.decode('ascii', errors='replace'), 'crc_and_endianness_pass': True,
              'P25_hardware_verified': False,
              'note': 'Power-cycle now; no flash/RF writes were requested. This is not an RF/codec timing test.'}
    with args.out.open('x') as dest:
        json.dump(result, dest, indent=2)
        dest.write('\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError) as exc:
        raise SystemExit(str(exc))
