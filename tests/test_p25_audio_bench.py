# SPDX-License-Identifier: GPL-3.0-or-later
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import MagicMock,patch
from tools import p25_audio_bench as bench
from tools.p25_audio_bench import allowed,capture
class AudioBenchTests(unittest.TestCase):
    def test_receive_only_no_tx_storage_or_registers(self):
        for cmd in (6,7,10,12,13,14,15,16,0x1a):self.assertFalse(allowed(cmd,b'\x00'))
        self.assertTrue(allowed(0x20,b'\x05'+struct.pack('<IHHH',145500000,60000,0x293,0)))
        self.assertFalse(allowed(0x21,b'\x01'+bytes(16)))
        for p in (b'\x00',b'\x08',b'\x09'):self.assertTrue(allowed(0x21,p))
        self.assertTrue(allowed(0x20,b'\x08'+struct.pack('<IHHH',145500000,300,0x293,0)))
        for seconds in (0,301):self.assertFalse(allowed(0x20,b'\x08'+struct.pack('<IHHH',145500000,seconds,0x293,0)))
    def test_replay_bounds_and_bad_payloads(self):
        for n in (1,64):self.assertTrue(allowed(0x20,b'\x02'+struct.pack('<H',n)))
        for n in (0,65,65535):self.assertFalse(allowed(0x20,b'\x02'+struct.pack('<H',n)))
        for p in (b'\x04',b'\x04\x02\x00',b'\x04\x01\x10',b'\x03'+bytes(40)):
            self.assertFalse(allowed(0x20,p))
        for ms in (0,999,60001):self.assertFalse(allowed(0x20,b'\x05'+struct.pack('<IHHH',145500000,ms,0x293,0)))
        for p in (b'\x09\x00',b'\x09\x01',b'\x0a'):self.assertTrue(allowed(0x20,p))
        for p in (b'\x09',b'\x09\x02',b'\x09\x01\x00',b'\x0a\x00'):self.assertFalse(allowed(0x20,p))
    def test_gap_and_unsupported_layout_rejected_before_device(self):
        with tempfile.TemporaryDirectory() as td:
            p=Path(td)/'rx.jsonl'
            rows=[dict(session=1,sequence=i,raw_hex=bytes(36).hex(),rx_type_info='0x0') for i in range(2)]
            p.write_text('\n'.join(json.dumps(r) for r in rows));self.assertEqual(len(capture(p)),2)
            rows[1]['sequence']=3;p.write_text('\n'.join(json.dumps(r) for r in rows))
            with self.assertRaisesRegex(ValueError,'gap'):capture(p)
            rows[1]['sequence']=1;rows[1]['rx_type_info']='0x3';p.write_text('\n'.join(json.dumps(r) for r in rows))
            with self.assertRaisesRegex(ValueError,'unsupported'):capture(p)
    def test_inspect_active_session_does_not_wait_or_stop_it(self):
        with tempfile.TemporaryDirectory() as td:
            path=Path(td)/'inspect.json';link=MagicMock();link.__enter__.return_value=link
            with patch.object(bench,'open_link',return_value=link),patch.object(bench,'status',return_value={'state':1}),patch.object(bench,'event_status',return_value={}),patch.object(bench,'playback_status',return_value={}),patch.object(bench.check,'restrictions',return_value={'flags':31}),patch.object(bench.time,'sleep',side_effect=AssertionError('Inspection must not wait for completion')),patch('builtins.print'),patch('sys.argv',['bench','--port','COM5','--out',str(path),'inspect']):
                link.view.return_value={};bench.main()
            self.assertTrue(json.loads(path.read_text())['pending'])
            link.request.assert_not_called()
    def test_long_receive_waits_for_new_active_session_before_returning(self):
        with tempfile.TemporaryDirectory() as td:
            path=Path(td)/'start.json';link=MagicMock();link.__enter__.return_value=link
            samples=[{'state':2,'native_session':1},
                     {'state':1,'native_state':2,'native_session':1},
                     {'state':1,'native_state':1,'native_session':2},
                     {'state':1,'native_state':2,'native_session':2}]
            with patch.object(bench,'open_link',return_value=link),patch.object(bench,'status',side_effect=samples),patch.object(bench,'event_status',return_value={}),patch.object(bench,'playback_status',return_value={}),patch.object(bench.check,'restrictions',return_value={'flags':31}),patch.object(bench.time,'sleep') as sleep,patch('builtins.print'),patch('sys.argv',['bench','--port','COM5','--out',str(path),'live','--duration-ms','300000','--no-wait']):
                link.view.return_value={'rx_hz':'145500000'};bench.main()
            self.assertEqual(sleep.call_count,2)
            link.request.assert_called_once_with(0x20,b'\x08'+struct.pack('<IHHH',145500000,300,0x293,0),busy_wait=0)
            self.assertTrue(json.loads(path.read_text())['passed'])
