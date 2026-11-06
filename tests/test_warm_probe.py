# SPDX-License-Identifier: GPL-3.0-or-later
import unittest
from tools import warm_probe as p
from tools import install_firstboot as install
from tools.dm32_readonly_backup import load_upstream
class ProbeTests(unittest.TestCase):
    def test_only_exact_restart_pokes_and_no_flash_arrays(self):
        m=load_upstream();g=p.ResetOnlyPort(None,m)
        for f in g.plan:
            with self.assertRaises(ValueError):g.validate(f)
        for payload in (b'\x06',b'\x20\x00\xc0\x00',b'\x02\x00\xc0\x00x',b'\x03\x00\xc0\x00'):
            with self.assertRaises(ValueError):g.validate(install.spi_packet(payload,1 if payload[0]==3 else 0))
        g.arm()
        with self.assertRaises(ValueError):g.validate(g.plan[-1])
        for f in g.plan:self.assertEqual(g.validate(f),'fixed_restart_register')
        self.assertEqual(g.index,7)
