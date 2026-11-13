# SPDX-License-Identifier: GPL-3.0-or-later
"""The firmware's NXDN48 chains against independent implementations (tools/nxdn_validate.py): our transmission
decoded by dsd-fme, and MMDVM's transmit shaping received by ours. Skipped until tools/build_validators.sh has built
dsd-fme and the protocol sources are fetched."""
from pathlib import Path
import contextlib
import io
import json
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import nxdn_validate  # noqa: E402


@unittest.skipUnless(nxdn_validate.DSD.exists() and nxdn_validate.MMDVM.exists(),
                     "dsd-fme or MMDVM missing (tools/build_validators.sh, tools/fetch_protocol_sources.sh)")
class NxdnIndependent(unittest.TestCase):
    def run_tool(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = nxdn_validate.main(list(args))
        return code, json.loads(out.getvalue())

    def test_call_both_ways(self):
        code, r = self.run_tool("--check", "--seconds", "2", "--ran", "25", "--source", "3210", "--group", "77")
        self.assertEqual(code, 0, r)
        nrz = r["dsd_fme_nrz"]["heard"]
        self.assertTrue(nrz["header_vcall_facch1"])
        self.assertEqual(nrz["ran"], [25])
        self.assertEqual(nrz["ambe_matching_ours"], r["dsd_fme_nrz"]["sent"]["ambe_frames"])
        rx = r["mmdvm_waveform_to_our_receiver"]["heard"]
        self.assertEqual((rx["source"], rx["destination"], rx["ran"]), (3210, 77, 25))

    def test_noisy_receive(self):
        code, r = self.run_tool("--check", "--seconds", "2", "--cnr-db", "12")
        self.assertEqual(code, 0, r)


if __name__ == "__main__":
    unittest.main()
