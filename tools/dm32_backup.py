#!/usr/bin/env python3
"""Bounded C7000 startup-loader flash reader, hardware-unverified.

Loads a hash-pinned, RAM-only patched helper with both flash-write dispatch
branches bypassed. It is research software, not a formally verified recovery
system. Read docs/recovery.md before live use.
"""
import argparse
import ast
import hashlib
import json
import os
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from experiments.modem.make_readonly_helper import patched_helper
ORIGINAL_HELPER_SHA256 = '59de726d5a50cf17573d35e98d183a184d3cc5da81ca30e198fb061cd3ee0f46'
HELPER_SHA256 = '8d316eff7b2be89745162da739cecc8334e2befcd278fa6748bff44cc6bfde42'
ACK = b'\x02\x05\x00\x03'
BOOT = b'\x02\x24\x00\x03'
PAGE = 256
CAPACITIES = {'program': (0x100000, 0x200000), 'data': (0x1000000,)}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def extract_helper(source):
    """AST literal extraction never imports or executes downloaded Python."""
    tree = ast.parse(source.read_text())
    candidates = [n for n in tree.body if isinstance(n, ast.Assign) and
                  any(isinstance(t, ast.Name) and t.id == 'firmwareBytes' for t in n.targets)]
    if len(candidates) != 1:
        raise ValueError('Expected exactly one firmwareBytes literal')
    chunks = ast.literal_eval(candidates[0].value)
    if not isinstance(chunks, list) or not all(isinstance(b, bytes) and len(b) == PAGE for b in chunks):
        raise ValueError('Malformed RAM helper chunks')
    helper = b''.join(chunks)
    if len(helper) != 3840 or sha(helper) != ORIGINAL_HELPER_SHA256:
        raise ValueError('Original RAM helper hash/length mismatch')
    helper = patched_helper(helper)
    if sha(helper) != HELPER_SHA256:
        raise ValueError('Read-only helper output hash mismatch')
    return helper


def packet(command, payload):
    return bytes([2, command, sum(payload) & 255, 3]) + payload


class Reader:
    def __init__(self, transport, io_timeout=3.0):
        self.port = transport
        self.io_timeout = io_timeout
        self.running = False

    def write(self, data):
        if self.port.write(data) != len(data):
            raise IOError('Short serial write')

    def exact(self, count):
        deadline = time.monotonic() + self.io_timeout
        result = bytearray()
        while len(result) < count:
            block = self.port.read(count - len(result))
            if block:
                result.extend(block)
            if len(result) < count and time.monotonic() >= deadline:
                raise TimeoutError(f'Short response: {len(result)}/{count} bytes')
        return bytes(result)

    def expect_ack(self):
        actual = self.exact(4)
        if actual != ACK:
            raise IOError('Unexpected helper ACK: ' + actual.hex())

    def start(self, helper, boot_timeout=30.0):
        if len(helper) != 3840 or sha(helper) != HELPER_SHA256:
            raise ValueError('RAM helper hash/length mismatch')
        deadline = time.monotonic() + boot_timeout
        window = bytearray()
        while time.monotonic() < deadline:
            b = self.port.read(1)
            if not b:
                continue
            window += b
            window = window[-4:]
            if bytes(window) == BOOT:
                break
        else:
            raise TimeoutError('No C7000 power-on handshake within the deadline')
        self.write(b'\x02\x14\x00\x03')
        time.sleep(0.05)
        # The upstream transfer pairs field 0x40 with 256 bytes, consistent
        # with 64 words. Full ROM length semantics still need target testing.
        for offset in range(0, len(helper), PAGE):
            payload = (0x10000 + offset).to_bytes(4, 'big') + (64).to_bytes(4, 'big') + helper[offset:offset + PAGE]
            self.write(packet(0x18, payload))  # RAM download while in startup loader
            self.expect_ack()
        self.write(packet(0x1b, (0x10974).to_bytes(4, 'big')))
        self.expect_ack()
        time.sleep(0.5)
        self.running = True

    def read_page(self, chip, address, capacity):
        if not self.running:
            raise RuntimeError('RAM helper has not been started')
        if chip not in CAPACITIES or capacity not in CAPACITIES[chip]:
            raise ValueError('Unrecognized chip capacity; identify actual flash first')
        if address < 0 or address % PAGE or address + PAGE > capacity:
            raise ValueError('Read exceeds identified chip range or is unaligned')
        # Read-only helper command subset; no 0x16/0x18 flash-write requests.
        self.write(packet(0x17 if chip == 'program' else 0x15, address.to_bytes(4, 'big')))
        reply = self.exact(4 + PAGE)
        # Unlike the fixed startup ACK, read replies put sum(data) mod 256 in
        # header byte2. Verified in pinned helper instructions 0x10d6e..0x10d88
        # and 0x10dc4..0x10dde, independently reviewed before hardware use.
        if reply[:2] != ACK[:2] or reply[3] != 3:
            raise IOError('Flash read failed at ' + hex(address) + ': ' + reply[:4].hex())
        if reply[2] != (sum(reply[4:]) & 255):
            raise IOError('Flash read checksum mismatch at ' + hex(address))
        return reply[4:]


def read_chip(reader, chip, capacity, destination):
    partial = destination.with_name(destination.name + '.partial')
    if destination.exists():
        raise FileExistsError(destination)
    digest = hashlib.sha256()
    pages = []
    # Retain incomplete data as .partial if interrupted. Never call it a backup.
    with partial.open('xb') as f:
        for address in range(0, capacity, PAGE):
            data = reader.read_page(chip, address, capacity)
            if len(data) != PAGE:
                raise IOError('Internal short-page error')
            f.write(data)
            digest.update(data)
            pages.append(sha(data))
            if address % 0x10000 == 0:
                print(f'{chip}: {address:#x}/{capacity:#x}', file=sys.stderr)
        f.flush()
        os.fsync(f.fileno())
    # Link creation is exclusive (unlike os.replace), preserving any racing file.
    os.link(partial, destination)
    partial.unlink()
    return {'file': destination.name, 'chip': chip, 'bytes': capacity,
            'sha256': digest.hexdigest(), 'page_bytes': PAGE, 'page_sha256': pages}


def compare_records(first, second):
    if first['chip'] != second['chip'] or first['bytes'] != second['bytes']:
        raise ValueError('Cannot compare different chips/ranges')
    count = first['bytes'] // PAGE
    if (first['bytes'] % PAGE or len(first['page_sha256']) != count or
            len(second['page_sha256']) != count):
        raise ValueError('Incomplete page-hash coverage')
    changed = [i * PAGE for i, (a, b) in enumerate(zip(first['page_sha256'], second['page_sha256'])) if a != b]
    return {'chip': first['chip'], 'identical': not changed,
            'changed_page_addresses': changed,
            'note': 'Investigate every change; no runtime/calibration regions are automatically ignored.'}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--helper-source', type=Path, default=ROOT / 'research/upstream/roger/python/C7000_read_progmem.py')
    ap.add_argument('--port', help='Explicit serial port; omitting it performs offline validation only')
    ap.add_argument('--program-bytes', type=lambda s: int(s, 0), choices=CAPACITIES['program'], required=True)
    ap.add_argument('--data-bytes', type=lambda s: int(s, 0), choices=CAPACITIES['data'], required=True)
    ap.add_argument('--identity', required=True, help='Local unit label + installed firmware; do not publish serial numbers')
    ap.add_argument('--installed-firmware', help='Exact version displayed by the unit; record before backup')
    ap.add_argument('--out', type=Path)
    ap.add_argument('--boot-timeout', type=float, default=30.0)
    args = ap.parse_args()
    helper = extract_helper(args.helper_source)
    if args.boot_timeout <= 0 or args.boot_timeout > 120:
        ap.error('Boot timeout must be in (0, 120] seconds')
    if args.port is None:
        print(json.dumps({'mode': 'offline-plan', 'helper_sha256': sha(helper),
                          'program_bytes': args.program_bytes, 'data_bytes': args.data_bytes,
                          'passes': 2, 'flash_write_commands': 0,
                          'hardware_verified': False}, indent=2))
        return
    if args.out is None:
        ap.error('--out is required for serial capture')
    # Refuse existing sessions, preserving all previous unit data.
    args.out.mkdir(parents=True, exist_ok=False)
    import serial
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=3)
    port.dtr = False
    port.rts = False
    port.port = args.port
    port.open()
    manifest = {'identity': args.identity, 'helper_sha256': sha(helper),
                'original_helper_sha256': ORIGINAL_HELPER_SHA256,
                'helper_source_file_sha256': sha(args.helper_source.read_bytes()),
                'helper_source_commit': '4b2185944a85a578ae33626922b763db6256533e',
                'installed_firmware': args.installed_firmware,
                'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                'captures': [], 'hardware_verified': False,
                'warning': 'Data/codeplug/calibration locations require separate identification.'}
    try:
        reader = Reader(port)
        print('Power on the radio with the cable seated for the startup-loader handshake.', file=sys.stderr)
        reader.start(helper, args.boot_timeout)
        for capture_pass in (1, 2):
            for chip, capacity in [('program', args.program_bytes), ('data', args.data_bytes)]:
                record = read_chip(reader, chip, capacity, args.out / f'{chip}-pass{capture_pass}.bin')
                record['pass'] = capture_pass
                manifest['captures'].append(record)
                (args.out / 'backup-manifest.partial.json').write_text(json.dumps(manifest, indent=2) + '\n')
        records = manifest['captures']
        manifest['comparison'] = [compare_records(records[i], records[i + 2]) for i in (0, 1)]
        manifest['capture_completed'] = True
        # Reading every requested address does not establish correct high-address
        # hardware access: check wrap/aliasing and JEDEC identity separately.
        (args.out / 'backup-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        print(json.dumps(manifest['comparison'], indent=2))
    finally:
        port.close()


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, RuntimeError) as exc:
        raise SystemExit(str(exc))
