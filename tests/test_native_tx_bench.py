import unittest
import struct
from unittest.mock import patch
from tools import native_tx_bench as b
class BenchTests(unittest.TestCase):
    def test_transport_excludes_restart_updater_registers_and_reset(self):
        arm=b'\x01'+struct.pack('<BBHHHII',2,1,4,1000,250,145500000,0x3532504e)
        self.assertTrue(b.allowed(0x21,arm))
        for cmd,p in ((13,bytes(5)),(0x1a,b'\x00'),(0x21,b'\x07'+bytes(4)),(0x21,arm[:-1]),(0x21,b'\x01'+struct.pack('<BBHHHII',2,1,4,1000,250,446050000,0x3532504e))):
            self.assertFalse(b.allowed(cmd,p))
    def health(self,flags=47,**fields):
        view={'state':'idle','mode':'fm','tx_hz':'145500000',**fields}
        return {'view':view,'restrictions':{'flags':flags}}
    def invoke(self,health,status):
        with patch.object(b.check,'collect',return_value=health),patch.object(b.native,'status',return_value=status),\
             patch.object(b.native,'run_session') as run:
            with self.assertRaises(ValueError):b.attempt(object(),[bytes(36)])
            run.assert_not_called()
    def test_missing_profile_channel_or_idle_never_starts(self):
        idle={'owned':0,'rx_count':0,'state':0}
        for health in (self.health(15),self.health(31),self.health(tx_hz='446050000'),self.health(state='rx'),self.health(mode='dmr')):
            self.invoke(health,idle)
        for status in ({**idle,'owned':1},{**idle,'state':6},{**idle,'rx_count':1}):self.invoke(self.health(),status)
    def test_single_scoped_attempt_has_no_retry_or_reset(self):
        with patch.object(b.check,'collect',return_value=self.health()),\
             patch.object(b.native,'status',return_value={'owned':0,'rx_count':0,'state':0}),\
             patch.object(b.native,'snapshot',return_value={}),\
             patch.object(b.native,'run_session',return_value={'cleanup_confirmed':True,'status':{'state':6,'reason':7}}) as run:
            r=b.attempt(object(),[bytes(36)]*4)
            run.assert_called_once()
            self.assertEqual(run.call_args.kwargs['frequency'],145500000)
            self.assertEqual(run.call_args.kwargs['duration'],1000)
            self.assertEqual(run.call_args.kwargs['lease'],250)
            self.assertEqual(r['reason_name'],'stalled')
    def test_bad_blocks_fail_before_health_or_hardware(self):
        with patch.object(b.check,'collect') as read:
            for data in ([],[b'bad'],[bytes(36)]*29):
                with self.assertRaises(ValueError):b.attempt(object(),data)
            read.assert_not_called()
