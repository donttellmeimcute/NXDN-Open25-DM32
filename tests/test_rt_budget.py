# SPDX-License-Identifier: GPL-3.0-or-later
"""The recorded real-time budget (results/rt-budget.json, tools/rt_budget.py): every mode below one CPU, and the
calibration consistent with the IMBE decoder's on-radio peak."""
from pathlib import Path
import json
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RealTimeBudget(unittest.TestCase):
    def test_every_mode_fits_and_the_calibration_holds(self):
        r = json.loads((ROOT / "results/rt-budget.json").read_text())
        for m in r["modes"]:
            self.assertLess(m["cpu_percent"], 80, m["mode"])
        est, peak = r["cross_check"]["imbe_decode_estimated_average_us"], r["cross_check"]["imbe_decode_radio_peak_us"]
        self.assertTrue(0.6 * peak <= est <= peak, (est, peak))   # an average below the measured peak, not far off


if __name__ == "__main__":
    unittest.main()
