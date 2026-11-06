# SPDX-License-Identifier: GPL-3.0-or-later
import unittest
from tools import receive_bench as b
class ReceiveBenchTests(unittest.TestCase):
    def test_native_transmit_and_load_refused(self):
        arm=bytearray([1,1,1])+bytearray(14)
        self.assertTrue(b.allowed(0x21,arm))
        arm[1]=2;self.assertFalse(b.allowed(0x21,arm))
        arm[1]=1;arm[-1]=1;self.assertFalse(b.allowed(0x21,arm))
        for payload in (bytes([2])+bytes(42),b'',b'\x03',bytes([8])+bytes(4)):
            self.assertFalse(b.allowed(0x21,payload))
    def test_no_control_storage_memory_or_reboot(self):
        for cmd in (6,7,10,12,13,14,15,16,0x1a,0x1b,0x1c,0x1d):
            self.assertFalse(b.allowed(cmd,b'\x00'))
        for cmd in (1,2,3,9,0x22):self.assertTrue(b.allowed(cmd,b'\x00'))
        for payload in (b'\x00',b'\x01'):self.assertTrue(b.allowed(0x20,payload))
        self.assertFalse(b.allowed(0x20,b'\x02'))
if __name__=='__main__':unittest.main()
