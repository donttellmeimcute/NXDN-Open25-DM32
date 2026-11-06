# SPDX-License-Identifier: GPL-3.0-or-later
import unittest
import json
from pathlib import Path
import tempfile
from unittest.mock import MagicMock,patch
from tools import cpu_bench as cpu
from tools.cpu_bench import delta
from tools.p25_audio_bench import allowed
class CpuBenchTests(unittest.TestCase):
    def test_accounting_units_and_irq_caveat(self):
        a={'cpu.us':1000000,'cpu.task.IDLE':700000,'cpu.task.p25-lab':200000}
        b={'cpu.us':6000000,'cpu.task.IDLE':4200000,'cpu.task.p25-lab':1700000}
        r=delta(a,b);self.assertEqual(r['rtos_attributed_busy_percent'],30)
        self.assertEqual(r['task_percent']['p25-lab'],30);self.assertTrue(r['irq_time_charged_to_interrupted_task'])
    def test_wrap_reset_or_changed_task_is_refused(self):
        a={'cpu.us':1000000,'cpu.task.IDLE':700000}
        for b in ({'cpu.us':900000,'cpu.task.IDLE':800000},
                  {'cpu.us':2000000,'cpu.task.IDLE':100},
                  {'cpu.us':2000000,'cpu.task.IDLE':900000,'cpu.task.new':100}):
            with self.assertRaises(ValueError):delta(a,b)
    def test_only_bounded_resource_and_check_commands_allowed(self):
        for p in (b'\x0b',b'\x0c\x00',b'\x0c\x01'):self.assertTrue(allowed(0x20,p))
        for p in (b'\x0b\x00',b'\x0c',b'\x0c\x02'):self.assertFalse(allowed(0x20,p))
    def test_existing_session_is_never_stopped(self):
        with tempfile.TemporaryDirectory() as td:
            out=Path(td)/'cpu.json';link=MagicMock();link.__enter__.return_value=link
            with patch.object(cpu.audio,'open_link',return_value=link),patch.object(cpu.audio,'status',return_value={'state':1}),patch.object(cpu.check,'restrictions',return_value={}),patch('sys.argv',['cpu','--port','COM5','--out',str(out)]):
                with self.assertRaisesRegex(ValueError,'Another audio session'):cpu.main()
            link.request.assert_not_called();self.assertFalse(json.loads(out.read_text())['passed'])
    def test_owned_rx_is_stopped_when_accounting_fails(self):
        with tempfile.TemporaryDirectory() as td:
            out=Path(td)/'cpu.json';link=MagicMock();link.__enter__.return_value=link
            link.view.return_value={'rx_hz':'145500000'}
            statuses=[{'state':2},{'native_session':1},{'state':1,'native_state':2,'native_session':2,'callbacks':2},{'state':2}]
            samples=[{'p25.bench':1},{'cpu.us':1000000,'cpu.task.IDLE':700000},{'cpu.us':900000,'cpu.task.IDLE':800000}]
            with patch.object(cpu.audio,'open_link',return_value=link),patch.object(cpu.audio,'status',side_effect=statuses),patch.object(cpu.check,'restrictions',return_value={}),patch.object(cpu,'snapshot',side_effect=samples),patch.object(cpu.time,'sleep'),patch('sys.argv',['cpu','--port','COM5','--out',str(out),'--workload','audio-idle']):
                with self.assertRaisesRegex(ValueError,'clock near wrap'):cpu.main()
            calls=link.request.call_args_list;self.assertEqual(len(calls),2)
            self.assertEqual(calls[0].args[1][0],5);self.assertEqual(calls[1].args,(0x20,b'\x06'))
