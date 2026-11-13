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
CFLAGS = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *SAN, "-Isrc/p25", "-Isrc/p25t", "-Isrc/p25p2",
          "-Itests/p25t"]
LIB = ["src/p25/p25.c", *sorted(str(p.relative_to(ROOT)) for d in ("src/p25t", "src/p25p2")
                               for p in (ROOT / d).glob("*.c"))]
# each test (under tests/) and the helpers it links
TESTS = {"p25t/test_tsbk": [], "p25p2/test_p2": [], "p25t/test_su": ["p25t/site.c"]}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, helpers in TESTS.items():
        extra = [f"tests/{h}" for h in helpers]
        exe = OUT / Path(name).name
        subprocess.run(["clang", *CFLAGS, f"tests/{name}.c", *extra, *LIB, "-lm", "-o", str(exe)], check=True, cwd=ROOT)
        subprocess.run([str(exe)], check=True, cwd=ROOT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
