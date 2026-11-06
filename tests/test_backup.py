import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('dm32_backup', Path(__file__).parents[1] / 'tools/dm32_backup.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class Port:
    def __init__(self, data=b'', fragment=1, short_write=False):
        self.data = bytearray(data)
        self.fragment = fragment
        self.writes = []
        self.short_write = short_write

    def read(self, count):
        count = min(count, self.fragment)
        result = self.data[:count]
        del self.data[:count]
        return bytes(result)

    def write(self, data):
        self.writes.append(data)
        return len(data) - int(self.short_write)


class BackupTests(unittest.TestCase):
    def test_fragmented_page_and_command(self):
        data = bytes(range(256))
        port = Port(bytes([2, 5, sum(data) & 255, 3]) + data, fragment=3)
        r = m.Reader(port)
        r.running = True
        self.assertEqual(r.read_page('program', 0x1fff00, 0x200000), bytes(range(256)))
        self.assertEqual(port.writes, [bytes.fromhex('02 17 1e 03 00 1f ff 00')])

    def test_data_read_command(self):
        port = Port(m.ACK + b'X' * 256, fragment=20)
        r = m.Reader(port)
        r.running = True
        r.read_page('data', 0xffff00, 0x1000000)
        self.assertEqual(port.writes[0][1], 0x15)

    def test_range_checks_no_io(self):
        port = Port()
        r = m.Reader(port)
        r.running = True
        for address, capacity in [(0x200000, 0x200000), (-256, 0x100000), (1, 0x100000), (0, 0x400000)]:
            with self.assertRaises(ValueError):
                r.read_page('program', address, capacity)
        self.assertFalse(port.writes)

    def test_bad_payload_checksum(self):
        r = m.Reader(Port(b'\x02\x05\x01\x03' + b'X' * 256, fragment=260))
        r.running = True
        with self.assertRaisesRegex(IOError, 'checksum mismatch'):
            r.read_page('data', 0, 0x1000000)

    def test_bad_reply_framing(self):
        r = m.Reader(Port(b'\x02\x05\x00\x04' + b'X' * 256, fragment=260))
        r.running = True
        with self.assertRaisesRegex(IOError, 'Flash read failed'):
            r.read_page('data', 0, 0x1000000)

    def test_short_reply_rejected(self):
        r = m.Reader(Port(m.ACK + b'X' * 12, fragment=100), io_timeout=0.001)
        r.running = True
        with self.assertRaises(TimeoutError):
            r.read_page('program', 0, 0x100000)

    def test_short_write_rejected(self):
        r = m.Reader(Port(short_write=True))
        with self.assertRaisesRegex(IOError, 'Short serial write'):
            r.write(b'123')

    def test_unstarted_no_io(self):
        r = m.Reader(Port())
        with self.assertRaises(RuntimeError):
            r.read_page('program', 0, 0x100000)

    def test_helper_ast_guard(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'source.py'
            p.write_text("raise RuntimeError('must never execute')\nfirmwareBytes = [b'X' * 256]\n")
            with self.assertRaises(ValueError):
                m.extract_helper(p)

    def test_boot_timeout(self):
        # Validation rejects wrong helpers before opening the wire.
        r = m.Reader(Port())
        with self.assertRaisesRegex(ValueError, 'helper hash'):
            r.start(b'X' * 3840, boot_timeout=0.001)

    def test_comparison_reports_all_changes(self):
        a = {'chip': 'program', 'bytes': 512, 'page_sha256': ['a', 'b']}
        b = dict(a, page_sha256=['a', 'c'])
        self.assertEqual(m.compare_records(a, b)['changed_page_addresses'], [256])

    def test_incomplete_hash_coverage(self):
        a = {'chip': 'program', 'bytes': 512, 'page_sha256': ['a', 'b']}
        b = dict(a, page_sha256=['a'])
        with self.assertRaisesRegex(ValueError, 'Incomplete'):
            m.compare_records(a, b)

    def test_partial_never_promoted(self):
        class FailingReader:
            def read_page(self, chip, address, capacity):
                if address >= 256:
                    raise TimeoutError('simulated cable loss')
                return b'X' * 256
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'backup.bin'
            with self.assertRaises(TimeoutError):
                m.read_chip(FailingReader(), 'program', 512, p)
            self.assertFalse(p.exists())
            self.assertEqual(p.with_name('backup.bin.partial').stat().st_size, 256)


if __name__ == '__main__':
    unittest.main()
