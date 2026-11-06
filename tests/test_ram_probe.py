import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import dm32_backup as backup
import run_ram_probe as m


TRANSCRIPT = (b'DM32-RAM-PROBE/1\r\ncrc16=000031c3\r\nbyte0=00000078\r\n'
              b'work_mode=00000013\r\nDONE; power-cycle to restore normal boot\r\n')


class Port:
    def __init__(self, reply):
        self.reply, self.writes = bytearray(reply), []
    def read(self, count):
        count = min(count, 7)
        out = bytes(self.reply[:count])
        del self.reply[:count]
        return out
    def write(self, data):
        self.writes.append(data)
        return len(data)


class ProbeTests(unittest.TestCase):
    def test_wire_order_no_execute_before_all_acks(self):
        port = Port(backup.BOOT + backup.ACK * 3 + TRANSCRIPT)
        blocks = [backup.packet(0x18, b'A' * 264), backup.packet(0x18, b'B' * 264)]
        execute = backup.packet(0x1b, (0x10000).to_bytes(4, 'big'))
        with patch.object(m.time, 'sleep'):
            self.assertEqual(m.execute_probe(port, blocks, execute), TRANSCRIPT)
        self.assertEqual(port.writes, [b'\x02\x14\x00\x03', *blocks, execute])

    def test_failed_download_never_executes(self):
        port = Port(backup.BOOT + bytes.fromhex('02 05 01 03'))
        blocks = [backup.packet(0x18, b'A' * 264)]
        with patch.object(m.time, 'sleep'), self.assertRaisesRegex(IOError, 'ACK'):
            m.execute_probe(port, blocks, b'execute-must-not-be-sent')
        self.assertNotIn(b'execute-must-not-be-sent', port.writes)

    def test_unordered_or_missing_workmode_not_success(self):
        bad = TRANSCRIPT.replace(b'work_mode=00000013\r\n', b'')
        port = Port(backup.BOOT + backup.ACK + bad)
        with patch.object(m.time, 'sleep'), self.assertRaisesRegex(IOError, 'complete expected output'):
            m.execute_probe(port, [], backup.packet(0x1b, (0x10000).to_bytes(4, 'big')))

    def test_caller_manifest_cannot_authorize_arbitrary_binary(self):
        with tempfile.TemporaryDirectory() as d:
            binary, manifest = Path(d) / 'probe.bin', Path(d) / 'probe.json'
            data = b'X' * 100
            binary.write_bytes(data)
            spec = dict(kind='ram-diagnostic-only', cpu='ck803s', load_address=0x10000,
                        entry_address=0x10000, endianness='little', abi='C-SKY ABIv2',
                        flash_writes=False, rf_writes=False, length=len(data), sha256=backup.sha(data))
            manifest.write_text(json.dumps(spec))
            with self.assertRaisesRegex(ValueError, 'hash/size'):
                m.probe_plan(binary, manifest)

    @unittest.skipUnless((ROOT / 'artifacts/startup-probe/probe.ram.bin').exists(), 'build probe first')
    def test_reviewed_probe_plan(self):
        spec, blocks, execute = m.probe_plan(ROOT / 'artifacts/startup-probe/probe.ram.bin',
                                            ROOT / 'artifacts/startup-probe/probe.manifest.json')
        self.assertEqual(spec['sha256'], m.REVIEWED_PROBE_SHA256)
        for i, block in enumerate(blocks):
            self.assertEqual(block[1], 0x18)
            self.assertEqual(block[2], sum(block[4:]) & 255)
            self.assertEqual(int.from_bytes(block[4:8], 'big'), 0x10000 + 256 * i)
            self.assertEqual(int.from_bytes(block[8:12], 'big'), 64)
            self.assertEqual(len(block), 268)
        self.assertEqual(execute, backup.packet(0x1b, (0x10000).to_bytes(4, 'big')))

    def test_backup_rejects_unproven_capture_or_wrong_helper(self):
        with tempfile.TemporaryDirectory() as d:
            manifest = Path(d) / 'backup-manifest.json'
            manifest.write_text(json.dumps({'capture_completed': False}))
            with self.assertRaisesRegex(ValueError, 'incomplete'):
                m.verify_backup(Path(d))
            manifest.write_text(json.dumps({'capture_completed': True}))
            with self.assertRaisesRegex(ValueError, 'provenance'):
                m.verify_backup(Path(d))

    def test_backup_aliases_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            directory = Path(d)
            path = directory / 'program-pass1.bin'
            data = b'\0' * 0x100000
            path.write_bytes(data)
            first = dict(chip='program', **{'pass': 1}, bytes=len(data),
                         file=path.name, sha256=backup.sha(data))
            second = dict(first, **{'pass': 2}, file='program-pass2.bin')
            manifest = dict(capture_completed=True, helper_sha256=backup.HELPER_SHA256,
                            original_helper_sha256=backup.ORIGINAL_HELPER_SHA256,
                            helper_source_commit='4b2185944a85a578ae33626922b763db6256533e',
                            installed_firmware='DM32.01.01.049',
                            captures=[first, second, {}, {}])
            record = directory / 'backup-manifest.json'
            record.write_text(json.dumps(manifest))
            os.link(path, directory / second['file'])
            with self.assertRaisesRegex(ValueError, 'without aliases'):
                m.verify_backup(directory)
            (directory / second['file']).unlink()
            (directory / second['file']).symlink_to(path)
            with self.assertRaisesRegex(ValueError, 'symlink'):
                m.verify_backup(directory)
            second['file'] = first['file']
            record.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, 'distinct generated'):
                m.verify_backup(directory)


if __name__ == '__main__':
    unittest.main()
