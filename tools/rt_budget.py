#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Worst-case real-time budget of every mode on the DM-32's CK803S (168 MHz), estimated on the host.

No radio is connected, so nothing here is timed on the CPU itself. tests/bench/rt_bench.c runs each mode's work for
one deadline unit (an IMBE frame, an LDU, an NXDN frame, a TSDU...) and callgrind counts the host instructions spent
inside it. One measurement made on the radio converts instructions into CK803S time: the IMBE encoder, the same
source, averaged 13.0 ms per 20 ms frame on the self-test waveform (PROGRESS.md, Phase 1 TX bench; the live call
averaged 13.3 ms, peak 18.6 ms). The ratio is applied to the other integer DSP workloads (FEC, filters, Viterbi,
Reed-Solomon); a different instruction mix can move the result, so the figures are estimates, with the IMBE decoder's
on-radio peak as a cross-check.

Not covered: the stock AMBE+2 vocoder of DMR and NXDN (not in this repository; NXDN runs it at DMR's rate, which
RadioFoundry already runs on this radio), RTOS overhead, and interrupt entry costs beyond the work counted.

    python3 tools/rt_budget.py [--json results/rt-budget.json]
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/rt-bench"
CODEC = ROOT / "build/rt-codec"
CPU_HZ = 168_000_000
# On the radio: IMBE encode of the self-test waveform, average per 20 ms frame (PROGRESS.md, Phase 1)
RADIO_ENCODE_US = 13_000
RADIO_DECODE_PEAK_US = 12_226      # docs/receive-audio.md: maximum IMBE block, saved Harris capture, no playback
ITER = {"imbe_enc": 64, "imbe_dec": 64, "p25_tx_ldu": 40, "p25_rx_ldu": 40, "nxdn_tx": 40, "nxdn_rx": 40,
        "tsbk_cc": 40, "tap_voice": 40, "su_75ms": 40, "p2_burst": 48}


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    if not (CODEC / "libdm32_imbe.a").exists():
        subprocess.run(["python3", "experiments/codec/build.py", "--out", str(CODEC)], check=True, cwd=ROOT,
                       stdout=subprocess.DEVNULL)
    src = ["tests/bench/rt_bench.c", "src/p25/p25.c", *sorted(str(p.relative_to(ROOT)) for d in ("src/p25t",
           "src/p25p2", "src/nxdn") for p in (ROOT / d).glob("*.c"))]
    objs = []
    for s in src:
        o = OUT / (Path(s).stem + ".o")
        subprocess.run(["clang", "-std=gnu11", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-Isrc/p25", "-Isrc/p25t",
                        "-Isrc/p25p2", "-Isrc/nxdn", "-Isrc/codec", "-c", s, "-o", str(o)], check=True, cwd=ROOT)
        objs.append(str(o))
    subprocess.run(["clang++", *objs, str(CODEC / "libdm32_imbe.a"), "-lm", "-o", str(OUT / "rt_bench")], check=True)


def count(name):
    n = ITER[name]
    run = subprocess.run(["valgrind", "--tool=callgrind", "--collect-atstart=no", f"--toggle-collect=bench_{name}",
                          f"--callgrind-out-file={OUT / ('cg.' + name)}", str(OUT / "rt_bench"), name, str(n)],
                         capture_output=True, text=True, check=True)
    m = re.search(r"Collected : (\d+)", run.stderr)
    return int(m.group(1)) / n


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args(argv)
    build()
    ir = {name: count(name) for name in ITER}
    cyc_per_ir = RADIO_ENCODE_US * 1e-6 * CPU_HZ / ir["imbe_enc"]
    us = {k: v * cyc_per_ir / CPU_HZ * 1e6 for k, v in ir.items()}
    # Per mode: the work per deadline period, as a share of the CPU
    modes = [
        ("P25 conventional TX", 180, {"imbe_enc": 9, "p25_tx_ldu": 1}, "measured on the radio: 13.3 ms average, 18.6 ms peak per 20 ms frame"),
        ("P25 conventional RX", 180, {"p25_rx_ldu": 1, "imbe_dec": 9}, f"on-radio IMBE peak {RADIO_DECODE_PEAK_US} us"),
        ("P25 trunk, control channel", 75, {"tsbk_cc": 1, "su_75ms": 1, "p25_rx_ldu": 360 / 864}, "the P25 framer also runs (counted with a whole LDU decode scaled, an overestimate)"),
        ("P25 trunk, voice RX", 180, {"p25_rx_ldu": 1, "imbe_dec": 9, "tap_voice": 1, "su_75ms": 180 / 75}, "P25 RX plus the control channel tap and the logic"),
        ("P25 trunk, voice TX", 180, {"imbe_enc": 9, "p25_tx_ldu": 1, "su_75ms": 180 / 75}, "P25 TX plus the logic"),
        ("NXDN RX (without the AMBE+2 decoder)", 80, {"nxdn_rx": 1}, "every compensation filter on; plus the stock AMBE+2 decoder at DMR's rate"),
        ("NXDN TX (without the AMBE+2 encoder)", 80, {"nxdn_tx": 1}, "framing, modulation, the timer interrupt's pulls; plus the stock AMBE+2 encoder"),
        ("Phase 2 receive (no PHY on the radio)", 30, {"p2_burst": 1}, "reference only"),
    ]
    rows = []
    for name, period_ms, parts, note in modes:
        busy = sum(us[k] * n for k, n in parts.items())
        rows.append({"mode": name, "period_ms": period_ms, "busy_us": round(busy), "cpu_percent": round(busy / (period_ms * 10), 1),
                     "parts": {k: round(us[k] * n) for k, n in parts.items()}, "note": note})
    report = {
        "method": "host instructions inside each workload (callgrind), converted with the on-radio IMBE encode average",
        "cpu_hz": CPU_HZ, "calibration": {"radio_imbe_encode_us": RADIO_ENCODE_US, "host_instructions": round(ir["imbe_enc"]),
                                          "ck803s_cycles_per_host_instruction": round(cyc_per_ir, 3)},
        "per_unit": {k: {"host_instructions": round(ir[k]), "estimated_us": round(us[k])} for k in ITER},
        "cross_check": {"imbe_decode_estimated_average_us": round(us["imbe_dec"]), "imbe_decode_radio_peak_us": RADIO_DECODE_PEAK_US},
        "modes": rows,
    }
    text = json.dumps(report, indent=2)
    print(text)
    if args.json:
        args.json.write_text(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
