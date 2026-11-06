#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline C4FM deviation oracle for independent measurements of native PHY TX.

TIA-102.BAAA-B 8.2 Table 20, 8.3-8.4.1 Figure 36; PDF 60-61,
printed 46-47, p25_d1460d9ae0713992. Full source identity and limits:
docs/conventional.md. This is a HOST test instrument, not an MCU modem.
"""
import argparse
import cmath
import csv
import hashlib
import json
import math
from pathlib import Path

RATE = 38400
SYMBOL_RATE = 4800
SPS = RATE // SYMBOL_RATE
SPAN = 24
LEVELS = (1, 3, -1, -3)


def response(f):
    f = abs(f)
    if f >= 2880:
        return 0.0
    h = 1.0 if f <= 1920 else 0.5 + 0.5 * math.cos(2 * math.pi * f / 1920)
    x = math.pi * f / SYMBOL_RATE
    return h * (x / math.sin(x) if x else 1.0)


def coefficients():
    # Direct numerical inverse transform of the specified combined H(f)P(f).
    # ±24-symbol truncation and 4096 integration intervals are implementation
    # choices; tests bound their spectral/calibration error, not RF compliance.
    bins = 4096
    df = 2880 / bins
    weights = [response(k * df) * (1 if k in (0, bins) else 2) for k in range(bins + 1)]
    half = SPAN * SPS
    taps = [sum(w * math.cos(2 * math.pi * k * df * n / RATE)
                for k, w in enumerate(weights)) * df / SYMBOL_RATE
            for n in range(-half, half + 1)]
    return taps


def modulate(dibits, taps=None):
    taps = coefficients() if taps is None else taps
    out = [0.0] * (len(dibits) * SPS + len(taps) - 1)
    for i, d in enumerate(dibits):
        if d not in (0, 1, 2, 3):
            raise ValueError("dibits must be integers 0..3")
        level = LEVELS[d] * 600
        for j, tap in enumerate(taps):
            out[i * SPS + j] += level * tap
    return out


def compare(expected, observed):
    if len(expected) != len(observed) or not expected:
        raise ValueError("aligned, equally sized nonempty captures required")
    n = len(expected)
    xmean, ymean = sum(expected) / n, sum(observed) / n
    var = sum((x - xmean) ** 2 for x in expected)
    if not var:
        raise ValueError("reference has no deviation variation")
    gain = sum((x - xmean) * (y - ymean) for x, y in zip(expected, observed)) / var
    offset = ymean - gain * xmean
    return {"samples": n, "sample_rate_hz": RATE,
            "direct_rmse_hz": math.sqrt(sum((x-y)**2 for x,y in zip(expected, observed))/n),
            "fitted_gain": gain, "fitted_offset_hz": offset,
            "residual_rmse_hz": math.sqrt(sum((y-gain*x-offset)**2 for x,y in zip(expected, observed))/n),
            "conformance": "not assessed; alignment, measurement chain, RF accuracy and normative tolerances require validation"}


def read_csv(path):
    with path.open() as f:
        return [float(row["deviation_hz"]) for row in csv.DictReader(f)]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="command", required=True)
    gen = sub.add_parser("generate")
    gen.add_argument("output", type=Path)
    gen.add_argument("--air", type=Path, help="packed MSB-first CAI bytes")
    gen.add_argument("--calibration-symbols", type=int, default=4800)
    comp = sub.add_parser("compare")
    comp.add_argument("reference", type=Path)
    comp.add_argument("capture", type=Path)
    args = ap.parse_args()
    if args.command == "compare":
        print(json.dumps(compare(read_csv(args.reference), read_csv(args.capture)), indent=2))
        return
    if args.air:
        data = args.air.read_bytes()
        dibits = [(b >> shift) & 3 for b in data for shift in (6, 4, 2, 0)]
        source = {"air_sha256": hashlib.sha256(data).hexdigest()}
    else:
        if args.calibration_symbols < 4:
            ap.error("need at least four calibration symbols")
        dibits = [1, 1, 3, 3] * (args.calibration_symbols // 4)
        source = {"pattern": "01 01 11 11", "symbols": len(dibits)}
    samples = modulate(dibits)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w") as f:
        w = csv.writer(f)
        w.writerow(("sample", "seconds", "deviation_hz"))
        for i, sample in enumerate(samples): w.writerow((i, f"{i/RATE:.9f}", f"{sample:.9f}"))
    args.output.with_suffix(".json").write_text(json.dumps({**source,
        "kind": "analytic host reference, no RF measurement", "sample_rate_hz": RATE,
        "group_delay_samples": SPAN * SPS, "tail_samples": SPAN * 2 * SPS,
        "waveform_includes_filter_transients": True, "source_notes": "docs/conventional.md"}, indent=2) + "\n")


if __name__ == "__main__": main()
