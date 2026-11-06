#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Two-pass DM32 backup through a pinned RAM monitor. No flash-write command.

The transport permits only the exact pinned RAM download, HELLO, timer read,
DM32 configuration, JEDEC IDs and bounded flash reads/CRCs. The upstream RAM
binary contains additional operations; this host guard never sends them.
No firmware installation, reboot, GPIO poke, baud change or RF command exists.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import time

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM_PIN = 'dfbaf20c400c73e2ccf6edb80af7d1d255bee418'
TOOL_SHA = '4e3ad1fa0c85d89e9f4bc15c5d6f97212bbab341ac72d958bdf7d0e250124b9e'
RAM_SHA = 'eae5b98615d67ba465061c84a5fb209a902333dc700671ce4ca69a05eb4aaee9'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def load_upstream():
    path = ROOT / 'research/upstream/radiofoundry/stub/stubtool.py'
    if sha(path.read_bytes()) != TOOL_SHA:
        raise ValueError('Pinned monitor host source changed; preserving it and refusing import')
    spec = importlib.util.spec_from_file_location('dm32_pinned_monitor', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if sha(module.stub_univ_image()) != RAM_SHA:
        raise ValueError('Pinned RAM monitor hash mismatch')
    return module


def capacity(jedec, memory):
    if len(jedec) != 3 or jedec[0] in (0, 255) or jedec[1] in (0, 255):
        raise ValueError('Missing/malformed flash JEDEC ID')
    allowed = (20, 21) if memory == 'progmem' else (24,) if memory == 'q128' else ()
    if jedec[2] not in allowed:
        raise ValueError(f'Unsupported {memory} JEDEC density {jedec.hex()}; do not assume a capacity')
    return 1 << jedec[2]


class ReadOnlyPort:
    """Validate every outgoing complete frame, including direct streamed reads."""
    def __init__(self, port, module):
        self.raw, self.m = port, module
        image = module.stub_univ_image()
        if sha(image) != RAM_SHA:
            raise ValueError('RAM image changed')
        image += b'\xff' * (-len(image) % 256)
        self.blocks = {module.rom_cmd(0x18, (0x10000+i).to_bytes(4, 'big') +
                       (64).to_bytes(4, 'big') + image[i:i+256]) for i in range(0, len(image), 256)}
        self.go = module.rom_cmd(0x1b, (0x10000).to_bytes(4, 'big'))
        self.sent = set()
        self.monitor = False
        self.limits = {'progmem': 0, 'q128': 0}
        self.counts = {}

    @property
    def timeout(self): return self.raw.timeout
    @timeout.setter
    def timeout(self, value): self.raw.timeout = value
    @property
    def baudrate(self): return self.raw.baudrate
    @baudrate.setter
    def baudrate(self, value):
        if value != 115200: raise ValueError('Backup transport is fixed at 115200 baud')
        self.raw.baudrate = value
    def read(self, count): return self.raw.read(count)
    def reset_input_buffer(self): self.raw.reset_input_buffer()
    def flush(self): self.raw.flush()

    def memory(self, payload):
        if len(payload) < 6: raise ValueError('Short memory selector')
        base, flags, cs = int.from_bytes(payload[:4], 'big'), payload[4], payload[5]
        if (base, flags, cs) == (0x04000000, 1, 0): return 'progmem'
        if (base, flags, cs) == (0x140a0000, 3, 18): return 'q128'
        raise ValueError('Unsupported SPI controller/chip select')

    def bounds(self, mem, address, length):
        if length <= 0 or address < 0 or address + length > self.limits[mem]:
            raise ValueError('Read outside identified flash capacity')

    def validate(self, data):
        if not self.monitor:
            if data == self.m.ROM_REPLY: return 'rom_greeting'
            if data in self.blocks:
                self.sent.add(data)
                return 'ram_download'
            if data == self.go and self.sent == self.blocks:
                self.monitor = True
                return 'ram_execute'
            raise ValueError('Only the exact pinned RAM image may be downloaded/executed')
        if len(data) < 6 or data[0] != 2 or data[5] != 3:
            raise ValueError('Malformed monitor frame')
        op, n = data[1], int.from_bytes(data[2:4], 'big')
        payload = data[6:]
        if n != len(payload) or data[4] != sum(payload) & 255:
            raise ValueError('Monitor frame length/checksum mismatch')
        if op == 1 and not payload: return 'hello'
        if op == 2 and payload == bytes.fromhex('140000180004'): return 'timer_read'
        if op == 0x0d and len(payload) == 16:
            a, b = struct.unpack('>II', payload[:8])
            if a == b and 1_000_000 < a < 400_000_000 and payload[8:] == bytes([255,255,33,32,1,0,3,0]):
                return 'dm32_config'
        if op == 5 and len(payload) >= 10:
            mem = self.memory(payload)
            tx_len, rx_len = struct.unpack('>HH', payload[6:10])
            tx = payload[10:]
            if tx_len == len(tx):
                if tx == b'\x9f' and rx_len == 3: return 'jedec_read'
                if len(tx) == 4 and tx[0] == 3 and 1 <= rx_len <= 1024:
                    self.bounds(mem, int.from_bytes(tx[1:], 'big'), rx_len)
                    return 'flash_read'
        if op in (7, 9) and len(payload) == 12:
            mem = self.memory(payload)
            address = int.from_bytes(payload[6:10], 'big')
            count = int.from_bytes(payload[10:12], 'big') if op == 7 else payload[10]
            if address % 4096 == 0 and 1 <= count <= 16 and (op == 7 or payload[11] in (0, 1)):
                self.bounds(mem, address, count * 4096)
                return 'sector_crc' if op == 7 else 'sector_read'
        raise ValueError(f'Monitor operation {op:#x} refused by read-only transport')

    def write(self, data):
        kind = self.validate(bytes(data))
        result = self.raw.write(data)
        if result is not None and result != len(data): raise OSError('Short serial write')
        self.counts[kind] = self.counts.get(kind, 0) + 1
        return result


def file_record(path, memory, size, capture_pass):
    data = path.read_bytes()
    if len(data) != size: raise ValueError('Incomplete capture')
    repeated_halves = data[:size//2] == data[size//2:]
    return {'file': path.name, 'memory': memory, 'bytes': size, 'pass': capture_pass,
            'sha256': sha(data), 'sector_bytes': 4096,
            'sector_sha256': [sha(data[i:i+4096]) for i in range(0, size, 4096)],
            'all_blank': data == b'\xff' * size,
            'suspicious_repeated_halves': repeated_halves and data != b'\xff' * size}


def capture(radio, memory, size, destination, capture_pass):
    partial = destination.with_suffix(destination.suffix + '.partial')
    if destination.exists(): raise FileExistsError(destination)
    last = [-1]
    def progress(done, total):
        percent = int(done * 100 / total)
        if percent // 5 != last[0]:
            last[0] = percent // 5
            print(f'{memory} pass {capture_pass}: {percent}% ({done}/{total})', flush=True)
    with partial.open('xb+') as out:
        radio.dump(out, memory, 0, size, progress=progress)
        out.flush(); os.fsync(out.fileno())
    record = file_record(partial, memory, size, capture_pass)
    # Exclusive promotion: never replace any previous completed capture.
    os.link(partial, destination); partial.unlink()
    record['file'] = destination.name
    return record


def compare(first, second):
    if (first['memory'], first['bytes']) != (second['memory'], second['bytes']):
        raise ValueError('Capture ranges differ')
    changed = [i*4096 for i, (a,b) in enumerate(zip(first['sector_sha256'], second['sector_sha256'])) if a != b]
    if len(first['sector_sha256']) != first['bytes']//4096 or len(second['sector_sha256']) != first['bytes']//4096:
        raise ValueError('Missing sector hashes')
    return {'memory': first['memory'], 'identical': not changed, 'changed_sector_addresses': changed}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port', required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--installed-firmware', required=True)
    ap.add_argument('--wait', type=int, default=120)
    args = ap.parse_args()
    if not 1 <= args.wait <= 120: ap.error('--wait must be 1..120 seconds')
    module = load_upstream()
    args.out.mkdir(parents=True, exist_ok=False)
    report = {'upstream_commit': UPSTREAM_PIN, 'host_tool_sha256': TOOL_SHA, 'ram_monitor_sha256': RAM_SHA,
              'installed_firmware_user_reported': args.installed_firmware, 'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),
              'flash_written': False, 'rf_command_sent': False, 'backup_verified': False, 'captures': []}
    import serial
    raw = serial.Serial(port=None, baudrate=115200, timeout=0.02, write_timeout=3)
    raw.dtr = raw.rts = False
    raw.port = args.port
    guard = ReadOnlyPort(raw, module)
    manifest = args.out / 'backup-manifest.partial.json'
    try:
        raw.open()
        print(f'READY: {args.port} open; switch radio OFF then ON normally, cable seated, no keys held. Waiting {args.wait}s.',flush=True)
        radio = module.Radio(guard, log=lambda message: print(message,flush=True), retries=3)
        radio.start(wait=args.wait)
        if not radio.v6: raise ValueError('Expected pinned universal monitor protocol')
        radio.configure('dm32')
        report['ram_monitor_footprint'] = radio.v6
        report['chips'] = {}
        for memory in ('progmem', 'q128'):
            jedec = radio.jedec(memory)
            size = capacity(jedec, memory)
            guard.limits[memory] = size
            report['chips'][memory] = {'jedec':jedec.hex(), 'bytes':size}
            print(f'{memory}: JEDEC {jedec.hex()}, {size} bytes',flush=True)
        # Board registry defaults to 1 MiB; a detected 2 MiB device must be read in full.
        radio.profile = dict(radio.profile, progmem=guard.limits['progmem'])
        manifest.write_text(json.dumps(report,indent=2)+'\n')
        for capture_pass in (1,2):
            for memory in ('progmem','q128'):
                record = capture(radio,memory,guard.limits[memory],args.out/f'{memory}-pass{capture_pass}.bin',capture_pass)
                report['captures'].append(record)
                manifest.write_text(json.dumps(report,indent=2)+'\n')
        report['comparison'] = [compare(report['captures'][i],report['captures'][i+2]) for i in (0,1)]
        report['backup_verified'] = all(c['identical'] for c in report['comparison']) and not any(
            c['all_blank'] or c['suspicious_repeated_halves'] for c in report['captures'])
        if not report['backup_verified']: raise ValueError('Captures changed, are blank, or suggest address aliasing; preserve and investigate')
        # Preserve the raw stock codeplug/calibration page area separately; not a vendor CPS export.
        data = (args.out/'q128-pass1.bin').read_bytes()
        codeplug = data[0x1000:0xc9000]
        with (args.out/'stock-codeplug-pages.raw.bin').open('xb') as out: out.write(codeplug)
        report['codeplug_raw'] = {'file':'stock-codeplug-pages.raw.bin','start':0x1000,'end_exclusive':0xc9000,
                                  'bytes':len(codeplug),'sha256':sha(codeplug),'vendor_cps_export':False}
        report['completed_utc'] = time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())
        report['outgoing_commands'] = guard.counts
        with (args.out/'backup-manifest.json').open('x') as out: json.dump(report,out,indent=2);out.write('\n')
        print('VERIFIED: both complete flash ranges read twice and matching. Power cycle to return to stock firmware.',flush=True)
    except BaseException as exc:
        report['backup_verified'] = False
        report['error'] = f'{type(exc).__name__}: {exc}'
        report['outgoing_commands'] = guard.counts
        manifest.write_text(json.dumps(report,indent=2)+'\n')
        raise
    finally:
        raw.close()


if __name__ == '__main__': main()
