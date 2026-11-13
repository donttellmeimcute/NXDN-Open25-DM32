# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/tx_trace_replay.py on traces built to the 2026-11-06 signature."""
import json
from pathlib import Path
import random
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import tx_trace_replay


def make_trace(blocks):
    return {'summary': {'version': 1, 'count': len(blocks)},
            'blocks': [{'us': 30000 * i, 'queued': 2, 'raw_hex': b.hex()} for i, b in enumerate(blocks)]}


class ReplayTest(unittest.TestCase):
    def setUp(self):
        rng = random.Random(20261106)
        self.intended = [bytes(rng.randrange(256) for _ in range(36)) for _ in range(192)]

    def run_tool(self, air_blocks, binary=False):
        with tempfile.TemporaryDirectory() as d:
            trace = Path(d) / 'trace.json'
            air = Path(d) / ('air.bin' if binary else 'air.txt')
            trace.write_text(json.dumps(make_trace(self.intended)))
            if binary:
                air.write_bytes(b''.join(air_blocks))
            else:
                air.write_text('\n'.join(b.hex() for b in air_blocks) + '\n')
            _, intended = tx_trace_replay.load_trace(trace)
            return tx_trace_replay.compare(intended, tx_trace_replay.load_air(air))

    def test_head_lag_signature(self):
        # Old write point: the first three bytes are the previous block's.
        air = [self.intended[0]] + [self.intended[k - 1][:3] + self.intended[k][3:] for k in range(1, 192)]
        r = self.run_tool(air)
        self.assertEqual(r['head_match_previous_pct'], 100.0)
        self.assertLess(r['head_match_intended_pct'], 40.0)
        self.assertEqual(r['blocks_exact'], 0)
        self.assertEqual(r['blocks_exact_after_head_substitution'], 191)
        self.assertTrue(r['verdict'].startswith('head-of-block lag'))

    def test_clean_handoff(self):
        r = self.run_tool(list(self.intended), binary=True)
        self.assertEqual(r['blocks_exact'], 191)
        self.assertEqual(r['head_match_intended_pct'], 100.0)
        self.assertEqual(r['verdict'], 'every compared block exact')

    def test_rejects_partial_blocks(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'air.bin'
            p.write_bytes(b'\0' * 35)
            with self.assertRaises(ValueError):
                tx_trace_replay.load_air(p)


if __name__ == '__main__':
    unittest.main()
