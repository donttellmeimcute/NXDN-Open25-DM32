# SPDX-License-Identifier: GPL-3.0-or-later
import struct
import unittest
from tools import firstboot_check as f
class Fake:
    def __init__(self):self.calls=[];self.flags=31;self.version_value='dfbaf20c-p25-firstboot';self.change=False
    def request(self,cmd,payload=b'',**kw):
        self.calls.append(cmd)
        if cmd==0x22:
            counts=[0]*len(f.COUNTERS)
            if self.change and len(self.calls)>1:counts[0]=1
            return 0,struct.pack('<'+'I'*(2+len(counts)),1,self.flags,*counts)
        return 0,payload
    def version(self):self.calls.append(2);return {'git':self.version_value}
    def view(self,sections):
        self.calls.append(9)
        return {'state':'idle','storage.sector_writes':'0','storage.claim_pending':'0'}
class ClientTests(unittest.TestCase):
    def test_only_health_queries_and_restrictions_first(self):
        link=Fake();self.assertTrue(f.collect(link)['health_receipt_passed'])
        self.assertEqual(link.calls,[0x22,1,2,9,0x22])
    def test_missing_inhibit_stops_before_other_queries(self):
        link=Fake();link.flags=30
        with self.assertRaises(ValueError):f.collect(link)
        self.assertEqual(link.calls,[0x22])
    def test_development_profiles_are_explicit(self):
        for flags,tx in ((15,False),(47,True)):
            link=Fake();link.flags=flags
            self.assertEqual(f.collect(link)['restrictions']['native_tx_bench_enabled'],tx)
    def test_wrong_image_or_counter_changes_fail(self):
        link=Fake();link.version_value='stock'
        with self.assertRaises(ValueError):f.collect(link)
        link=Fake();link.change=True
        with self.assertRaises(ValueError):f.collect(link)
    def test_partial_error_and_version_mismatch(self):
        for status,data in ((0,b''),(7,b''),(0,struct.pack('<13I',2,31,*([0]*11)))):
            class Bad:
                def request(self,*args,**kw):return status,data
            with self.assertRaises(ValueError):f.restrictions(Bad())
if __name__=='__main__':unittest.main()
