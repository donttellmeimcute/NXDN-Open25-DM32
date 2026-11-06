# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import cmath
import math
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("c4fm_reference", Path(__file__).resolve().parents[1] / "tools/c4fm_reference.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class WaveformTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.taps = m.coefficients()

    def test_mapping_and_response(self):
        self.assertEqual(m.LEVELS, (1, 3, -1, -3))
        for f in (0, 600, 1200, 1920, 2200, 2400, 2700, 2880, 3200, 4000):
            actual = abs(sum(t * cmath.exp(-2j * math.pi * f * i / m.RATE) for i,t in enumerate(self.taps))) / m.SPS
            self.assertLess(abs(actual - m.response(f)), 0.0015, f)

    def test_tia_calibration_pattern(self):
        # Independently known fundamental amplitude: 1800*pi/2 at 1200 Hz.
        signal = m.modulate([1, 1, 3, 3] * 40, self.taps)
        start = m.SPAN * m.SPS * 2
        block = signal[start:start + 32 * 20]
        fundamental = 2 * abs(sum(x * cmath.exp(-2j * math.pi * i / 32) for i,x in enumerate(block))) / len(block)
        self.assertLess(abs(fundamental - 1800 * math.pi / 2), 2)
        self.assertLess(abs(sum(block) / len(block)), 0.01)

    def test_measurement_does_not_hide_wrong_deviation(self):
        x = m.modulate([0, 1, 2, 3] * 20, self.taps)
        r = m.compare(x, [a * 1.08 + 125 for a in x])
        self.assertAlmostEqual(r["fitted_gain"], 1.08)
        self.assertAlmostEqual(r["fitted_offset_hz"], 125)
        self.assertLess(r["residual_rmse_hz"], 1e-8)
        self.assertGreater(r["direct_rmse_hz"], 125)
        with self.assertRaises(ValueError): m.compare(x, x[:-1])


if __name__ == "__main__": unittest.main()
