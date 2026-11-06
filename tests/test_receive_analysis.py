# SPDX-License-Identifier: GPL-3.0-or-later
import ctypes as C
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('receive_analysis',ROOT/'experiments/modem/analyze_capture.py')
analysis=importlib.util.module_from_spec(spec);spec.loader.exec_module(analysis)
from stream_probe import rx_ram_from_air

class Call(C.Structure):
    _fields_=[('nac',C.c_uint16),('talkgroup',C.c_uint16),('source_id',C.c_uint32),
              ('manufacturer_id',C.c_uint8),('service_options',C.c_uint8),('status_symbol',C.c_uint8)]

class ReceiveAnalysisTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory()
        path=Path(cls.temp.name)/'cai.so'
        subprocess.run(['cc','-shared','-fPIC','-O2','-I'+str(ROOT/'src/p25'),
                        str(ROOT/'src/p25/p25.c'),'-o',str(path)],check=True)
        cls.lib=analysis.library(path)
        cls.lib.p25_encode_hdu.argtypes=[C.POINTER(Call),C.c_void_p]
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def fixture(self):
        out=(C.c_uint8*99)()
        self.assertEqual(self.lib.p25_encode_hdu(C.byref(Call(0x293,1,123,0,0,0)),out),0)
        # Synthetic prefix/padding completes RAM records; analyzer never pads captures.
        air=bytes(out)+bytes(9)
        return [dict(session=1,sequence=i,tick=i*30000,sys_flags='0x800',rx_type_info='0x0',
            raw_hex=rx_ram_from_air(air[i*36:(i+1)*36],0).hex()) for i in range(3)]
    def test_complete_hdu_fec(self):
        result=analysis.analyze(self.fixture(),self.lib)
        found=[c for s in result['segments'] for c in s['candidates'] if c['layout']=='fig70' and c['mapping']=='identity']
        self.assertEqual(len(found),1)
        self.assertEqual(found[0]['nac'],'0x293')
        self.assertTrue(found[0]['metadata_valid'])
        self.assertEqual(found[0]['metadata_corrected_symbols'],0)
    def test_incomplete_is_not_padded(self):
        result=analysis.analyze(self.fixture()[:2],self.lib)
        found=[c for s in result['segments'] for c in s['candidates'] if c['mapping']=='identity']
        self.assertEqual(len(found),1)
        self.assertFalse(found[0]['complete_frame_available'])
        self.assertNotIn('metadata_valid',found[0])
    def test_sequence_gap_cannot_make_complete_frame(self):
        rows=self.fixture();rows[2]['sequence']=3
        result=analysis.analyze(rows,self.lib)
        self.assertEqual(len(result['segments']),2)
        self.assertFalse(any(c['complete_frame_available'] for s in result['segments'] for c in s['candidates']))
    def test_session_boundary_cannot_make_complete_frame(self):
        rows=self.fixture();rows[2]['session']=2
        self.assertEqual(len(analysis.analyze(rows,self.lib)['segments']),2)

if __name__=='__main__':unittest.main()
