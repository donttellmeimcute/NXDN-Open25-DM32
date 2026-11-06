# SPDX-License-Identifier: GPL-3.0-or-later
import unittest
from unittest.mock import Mock,patch
from tools import normal_reboot as r
from tools.normal_reboot import allowed
class RebootTests(unittest.TestCase):
    def test_normal_restart_only(self):
        self.assertTrue(allowed(13,b'\x00'+bytes(4)))
        for p in (b'',b'\x01'+bytes(4),bytes(4),bytes(6)):self.assertFalse(allowed(13,p))
    def test_no_tx_memory_writes_or_experiments(self):
        for cmd in (6,7,10,12,14,15,16,0x1a):self.assertFalse(allowed(cmd,b'\x00'))
        for cmd in (0x20,0x21,0x22):
            self.assertTrue(allowed(cmd,b'\x00'));self.assertFalse(allowed(cmd,b'\x01'))
    def test_restart_checks_busy_workers_and_sends_only_normal_mode(self):
        link=Mock();link.request.return_value=(0,b'p25.state=0\n')
        link.view.return_value={'ms':10000};link.chip.return_value={'token':123}
        with patch.object(r.check,'restrictions'),patch.object(r.native,'status',return_value={'owned':1,'state':2}):
            with self.assertRaisesRegex(ValueError,'Native'):r.request_restart(link)
        link.reboot.assert_not_called()
        with patch.object(r.check,'restrictions'),patch.object(r.native,'status',return_value={'owned':0,'state':5}):
            link.request.return_value=(0,b'p25.state=1\n')
            with self.assertRaisesRegex(ValueError,'Codec'):r.request_restart(link)
            link.reboot.assert_not_called();link.request.return_value=(0,b'p25.state=0\n')
            self.assertEqual(r.request_restart(link),{'ms':10000})
        link.reboot.assert_called_once_with(0,123)
if __name__=='__main__':unittest.main()
