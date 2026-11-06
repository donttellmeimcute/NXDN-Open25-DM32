# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest
from tools import dm32_update_probe as p
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('reference_loader',ROOT/'research/upstream/radiofoundry/tools/serial_loader.py')
reference=importlib.util.module_from_spec(spec);spec.loader.exec_module(reference)


class ProbeTests(unittest.TestCase):
    def test_real_reference_loader_stops_before_programming(self):
        fake=reference.FakeBootloader(reference.MODELS['dm32'])
        report=p.identify(fake,timeout=0.01)
        self.assertEqual(report['requests_hex'],['52','4d00000001','06'])
        self.assertEqual(fake.state,4);self.assertIsNone(fake.header);self.assertFalse(fake.blocks)
    def test_bad_protocol_stops_before_model_ack(self):
        fake=reference.FakeBootloader(reference.MODELS['dm32'],bad_step2=True)
        with self.assertRaises(ValueError):p.identify(fake,timeout=0.01)
        self.assertFalse(fake.blocks);self.assertIsNone(fake.header)
    def test_other_model_and_silent_port(self):
        fake=reference.FakeBootloader(SimpleNamespace(magic=b'OTHER-LDR'))
        with self.assertRaises(ValueError):p.identify(fake,timeout=0.01)
        with self.assertRaises(TimeoutError):p.identify(reference.FakeBootloader(reference.MODELS['dm32'],silent=True),timeout=0.01)
    def test_transport_refuses_erase_data_and_fourth_step(self):
        fake=reference.FakeBootloader(reference.MODELS['dm32']);guard=p.ProbePort(fake)
        for value in (b'\x45\0\0\0\0',b'\x31',b'\x01',b'\x00\xff'+bytes(130),b'\x04'):
            with self.assertRaises(ValueError):guard.write(value)
        for value in (b'\x52',bytes.fromhex('4d00000001'),b'\x06'):guard.write(value)
        with self.assertRaises(ValueError):guard.write(bytes.fromhex('4500000000'))
        self.assertIsNone(fake.header);self.assertFalse(fake.blocks)


if __name__=='__main__':unittest.main()
