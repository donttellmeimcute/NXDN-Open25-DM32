# SPDX-License-Identifier: GPL-3.0-or-later
"""P25 Phase 2 above the physical layer against dsd-fme and OP25's Python TDMA decoder (tools/p25p2_validate.py).
Skipped without them."""
from pathlib import Path
import contextlib
import io
import json
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import p25p2_validate  # noqa: E402


@unittest.skipUnless(p25p2_validate.DSD.exists() and (p25p2_validate.TDMA / "tdma-decode.py").exists()
                     and (p25p2_validate.AMBE / "ambeFec.c").exists(),
                     "dsd-fme, OP25 or the RadioFoundry tree missing (tools/build_validators.sh, "
                     "tools/fetch_protocol_sources.sh, tools/build_radiofoundry.py)")
class Phase2(unittest.TestCase):
    def test_dsd_fme_and_op25_read_our_voice_channel(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = p25p2_validate.main(["--check"])
        r = json.loads(out.getvalue())
        self.assertEqual(code, 0, r)
        for decoder in ("dsd_fme", "op25_tdma_python"):
            for c in (0, 1):
                ch = r[decoder]["channel_%d" % c]
                self.assertEqual(ch["frames_matching"], ch["frames_sent"])


if __name__ == "__main__":
    unittest.main()
