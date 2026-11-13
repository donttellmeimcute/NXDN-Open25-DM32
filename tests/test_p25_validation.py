# SPDX-License-Identifier: GPL-3.0-or-later
"""The firmware's P25 TX chain decoded by dsd-fme (tools/p25_validate.py). Skipped until
tools/build_validators.sh has built dsd-fme."""
from pathlib import Path
import contextlib
import io
import json
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import p25_validate  # noqa: E402


@unittest.skipUnless(p25_validate.DSD.exists(), "dsd-fme not built (tools/build_validators.sh)")
class P25IndependentDecode(unittest.TestCase):
    def run_tool(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = p25_validate.main(list(args))
        return code, json.loads(out.getvalue())

    def test_call_bit_exact(self):
        code, r = self.run_tool("--check", "--seconds", "2", "--talkgroup", "4242", "--source", "7654321", "--nac", "A5B")
        self.assertEqual(code, 0, r)
        heard = r["dsd_fme"]
        self.assertEqual(heard["frames"], r["transmitted"]["tx_frames"])
        self.assertEqual((heard["hdu"], heard["tdu"]), (1, 1))
        self.assertEqual(heard["nac"], ["A5B"])
        self.assertEqual(heard["imbe_matching_ours"], r["transmitted"]["imbe_frames"])

    def test_noisy_channel(self):
        code, r = self.run_tool("--check", "--seconds", "2", "--snr-db", "12")
        self.assertEqual(code, 0, r)


if __name__ == "__main__":
    unittest.main()
