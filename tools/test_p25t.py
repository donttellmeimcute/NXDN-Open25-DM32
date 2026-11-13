#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""P25 trunking host tests (src/p25t): signalling blocks, messages, the subscriber's trunking logic against a
simulated site, and Phase 2 above the physical layer. ASan/UBSan. No radio."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/p25t-host"
SAN = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
CFLAGS = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *SAN, "-Isrc/p25", "-Isrc/p25t"]
LIB = ["src/p25/p25.c", *sorted(str(p.relative_to(ROOT)) for p in (ROOT / "src/p25t").glob("*.c"))]
TESTS = ["test_tsbk"]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name in TESTS:
        extra = sorted(str(p.relative_to(ROOT)) for p in (ROOT / "tests/p25t").glob(name + "_*.c"))
        subprocess.run(["clang", *CFLAGS, f"tests/p25t/{name}.c", *extra, *LIB, "-lm", "-o", str(OUT / name)],
                       check=True, cwd=ROOT)
        subprocess.run([str(OUT / name)], check=True, cwd=ROOT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
