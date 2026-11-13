# SPDX-License-Identifier: GPL-3.0-or-later
"""The trunking signalling against OP25's TSBK parser and dsd-fme (tools/p25t_validate.py). Skipped without them."""
from pathlib import Path
import contextlib
import io
import json
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import p25t_validate  # noqa: E402


@unittest.skipUnless((p25t_validate.OP25_APPS / "tk_p25.py").exists() and p25t_validate.DSD.exists(),
                     "OP25 or dsd-fme missing (tools/fetch_protocol_sources.sh, tools/build_validators.sh)")
class TrunkingSignalling(unittest.TestCase):
    def test_op25_and_dsd_fme_read_our_tsbks(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = p25t_validate.main(["--check"])
        r = json.loads(out.getvalue())
        self.assertEqual(code, 0, r)
        self.assertEqual(r["op25_tk_p25"]["matching"], r["op25_tk_p25"]["tsbks"])
        self.assertTrue(r["dsd_fme_control_channel"]["match"])


if __name__ == "__main__":
    unittest.main()
