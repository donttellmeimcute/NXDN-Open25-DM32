# SPDX-License-Identifier: GPL-3.0-or-later
import struct
import unittest
from unittest.mock import patch
from tools import warm_update as w
from tools import native_modem
class Protocol:
    def request(self,cmd,payload=b'',**kw):
        self.calls.append((cmd,payload))
        if cmd==0x22:return 0,struct.pack('<13I',1,self.flags,*([0]*11))
        if cmd==0x21:
            values=[0]*len(native_modem.FIELDS);values[0]=1;return 0,struct.pack('<37I',*values)
        if cmd==0x20:return 0,f'p25.state={self.codec}\n'.encode()
        if payload==b'\x00':return 0,struct.pack('<IIIHBI',w.MAGIC,w.BASE,0x14000,59,0,0)
        if payload[0]==1:self.received=0;return 0,bytes(4)
        if payload[0]==2:
            self.received+=len(payload)-5
            return 0,struct.pack('<I',self.received+int(self.corrupt))
        return 0,b''
class Link(Protocol):
    def __init__(self,flags=15,codec=0,corrupt=False):self.flags=flags;self.codec=codec;self.corrupt=corrupt;self.calls=[]
    def view(self,sections):return {'state':'idle'}
class WarmUpdateTests(unittest.TestCase):
    def invoke(self,link):
        with patch.object(w.normal_reboot,'open_link',return_value=link),patch.object(w.time,'sleep'):
            return w.enter('COM5',object(),lambda s:None)
    def test_exact_pinned_helper_no_flash_operation(self):
        link=Link();r=self.invoke(link)
        self.assertEqual(r['helper_bytes'],7784);self.assertFalse(r['preinstall_reads_requested'])
        ops=[p[0] for c,p in link.calls if c==0x1a];self.assertEqual(ops[:2],[0,1]);self.assertEqual(ops[-1],3)
        self.assertTrue(all(c in (0x20,0x21,0x22,0x1a) for c,p in link.calls))
    def test_wrong_image_or_busy_codec_never_uploads(self):
        for link in (Link(flags=31),Link(codec=1)):
            with self.assertRaises(ValueError):self.invoke(link)
            self.assertFalse(any(c==0x1a for c,p in link.calls))
    def test_bad_ack_never_enters_helper(self):
        link=Link(corrupt=True)
        with self.assertRaisesRegex(ValueError,'DATA acknowledgment'):self.invoke(link)
        self.assertFalse(any(c==0x1a and p[0]==3 for c,p in link.calls))
    def test_scoped_tx_profile_can_update_only_while_idle(self):
        link=Link(flags=47);self.assertEqual(self.invoke(link)['helper_bytes'],7784)
