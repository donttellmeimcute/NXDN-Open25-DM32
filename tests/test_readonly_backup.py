# SPDX-License-Identifier: GPL-3.0-or-later
import io
from pathlib import Path
import tempfile
import unittest
from tools import dm32_readonly_backup as b


class Raw:
    baudrate=115200
    timeout=0.02
    def __init__(self): self.sent=[]
    def write(self,data): self.sent.append(bytes(data)); return len(data)


class BackupGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.m=b.load_upstream()
    def setUp(self):
        self.raw=Raw();self.guard=b.ReadOnlyPort(self.raw,self.m)
    def frame(self,op,payload=b''):
        return bytes([2,op])+len(payload).to_bytes(2,'big')+bytes([sum(payload)&255,3])+payload
    def running(self):
        for packet in self.guard.blocks:self.guard.write(packet)
        self.guard.write(self.guard.go)
    def test_rom_exact_image_and_execute_only_after_all_blocks(self):
        with self.assertRaises(ValueError):self.guard.write(self.guard.go)
        with self.assertRaises(ValueError):self.guard.write(self.m.rom_cmd(0x18,bytes(264)))
        self.guard.write(self.m.ROM_REPLY);self.running()
        self.assertEqual(self.guard.counts['ram_execute'],1)
        with self.assertRaises(ValueError):self.guard.write(self.m.ROM_REPLY)
    def test_capacity_and_unknown_id(self):
        self.assertEqual(b.capacity(bytes.fromhex('c84014'),'progmem'),0x100000)
        self.assertEqual(b.capacity(bytes.fromhex('c84015'),'progmem'),0x200000)
        self.assertEqual(b.capacity(bytes.fromhex('ef4018'),'q128'),0x1000000)
        for data,mem in [(b'\xff'*3,'q128'),(bytes.fromhex('c84018'),'progmem'),(bytes.fromhex('c84015'),'q128'),(b'12','q128')]:
            with self.assertRaises(ValueError):b.capacity(data,mem)
    def test_blocks_all_other_operations_and_bad_config(self):
        self.running();before=len(self.raw.sent)
        for op in (0,3,4,6,8,10,11,12,14,15,0x18,0x1b,255):
            with self.assertRaises(ValueError):self.guard.write(self.frame(op))
        with self.assertRaises(ValueError):self.guard.write(self.frame(13,bytes(16)))
        self.assertEqual(len(self.raw.sent),before)
        with self.assertRaises(ValueError):self.guard.baudrate=262500
    def test_spi_read_only_and_capacity_bounds(self):
        self.running();prefix=bytes.fromhex('040000000100')
        def spi(tx,n=3):return self.frame(5,prefix+len(tx).to_bytes(2,'big')+n.to_bytes(2,'big')+tx)
        self.guard.write(spi(b'\x9f'))
        with self.assertRaises(ValueError):self.guard.write(spi(bytes.fromhex('03000000'),16))
        self.guard.limits['progmem']=0x200000
        self.guard.write(spi(bytes.fromhex('031ffff0'),16))
        for tx,n in [(bytes.fromhex('031ffff0'),17),(b'\x06',0),(bytes.fromhex('20000000'),0),(bytes.fromhex('0200000000'),0)]:
            before=len(self.raw.sent)
            with self.assertRaises(ValueError):self.guard.write(spi(tx,n))
            self.assertEqual(len(self.raw.sent),before)
        wrong=self.frame(5,bytes.fromhex('140a00000311000100039f'))
        with self.assertRaises(ValueError):self.guard.write(wrong)
    def test_stream_crc_boundaries_and_corruption(self):
        self.running();self.guard.limits['q128']=0x1000000
        prefix=bytes.fromhex('140a00000312')
        self.guard.write(self.frame(9,prefix+bytes.fromhex('00fff0000101')))
        self.guard.write(self.frame(7,prefix+bytes.fromhex('00fff0000001')))
        for payload in (prefix+bytes.fromhex('00fff0000201'),prefix+bytes.fromhex('000000010101'),prefix+bytes.fromhex('000000001101')):
            with self.assertRaises(ValueError):self.guard.write(self.frame(9,payload))
        damaged=bytearray(self.frame(1));damaged[4]^=1
        with self.assertRaises(ValueError):self.guard.write(damaged)
    def test_real_upstream_dump_on_guarded_fake_radio(self):
        program=bytes(range(256))*32;data=b'\xff'*4096+bytes(range(255,-1,-1))*16
        fake=self.m.FakeRadio({'progmem':program,'q128':data},stub_len=8192)
        fake.running=True;fake.write_op=True
        guard=b.ReadOnlyPort(fake,self.m);guard.monitor=True
        guard.limits={'progmem':len(program),'q128':len(data)}
        radio=self.m.Radio(guard,log=lambda *a:None,retries=1)
        radio.sector_op=True;radio.profile=dict(self.m.C7_BOARDS['dm32'],progmem=len(program))
        radio.v6={'flags':0}
        for mem,expected in [('progmem',program),('q128',data)]:
            out=io.BytesIO();radio.dump(out,mem,0,len(expected));self.assertEqual(out.getvalue(),expected)
        self.assertFalse(fake.writes);self.assertFalse(fake.wel);self.assertFalse(fake.pokes)
        self.assertIn('sector_read',guard.counts);self.assertIn('sector_crc',guard.counts)
    def test_matching_changed_blank_and_alias_records(self):
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder)/'capture.bin';p.write_bytes(b'a'*4096+b'b'*4096)
            first=b.file_record(p,'progmem',8192,1)
            self.assertFalse(first['suspicious_repeated_halves'])
            second=b.file_record(p,'progmem',8192,2);self.assertTrue(b.compare(first,second)['identical'])
            p.write_bytes(b'a'*4096+b'c'*4096);second=b.file_record(p,'progmem',8192,2)
            self.assertEqual(b.compare(first,second)['changed_sector_addresses'],[4096])
            p.write_bytes(b'a'*8192);self.assertTrue(b.file_record(p,'progmem',8192,1)['suspicious_repeated_halves'])
            p.write_bytes(b'\xff'*8192);self.assertTrue(b.file_record(p,'progmem',8192,1)['all_blank'])
    def test_existing_capture_is_preserved(self):
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder)/'old.bin';p.write_bytes(b'original')
            with self.assertRaises(FileExistsError):b.capture(None,'progmem',4096,p,1)
            self.assertEqual(p.read_bytes(),b'original')


if __name__=='__main__':unittest.main()
