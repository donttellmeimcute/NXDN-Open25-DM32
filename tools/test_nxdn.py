#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""NXDN48 host tests: frame and call layers (ASan/UBSan), the modem and channel simulation, the link (TX ring and
RX path the radio drives), and the frame layer
against MMDVMHost's classes when research/protocol/MMDVMHost is present (tools/build_validators.sh). No radio."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/nxdn-host"
RF = ROOT / "build/radiofoundry-p25/core"
MMDVM = ROOT / "research/protocol/MMDVMHost"
SAN = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
CFLAGS = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *SAN, "-I" + str(ROOT / "src/nxdn")]
LIB = [ROOT / "src/nxdn/nxdn.c", ROOT / "src/nxdn/nxdn_call.c", ROOT / "src/nxdn/nxdn_modem.c", ROOT / "src/nxdn/nxdn_link.c"]


def run(args, **kw):
    subprocess.run([str(a) for a in args], check=True, cwd=ROOT, **kw)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    if not RF.exists():
        run(["python3", "tools/build_radiofoundry.py", "--prepare-only"])
    run(["clang", *CFLAGS, "tests/nxdn/test_nxdn_call.c", *LIB, "-lm", "-o", OUT / "test_nxdn_call"])
    run([OUT / "test_nxdn_call"])
    run(["clang", *CFLAGS, "-I" + str(RF), "tests/nxdn/test_nxdn_modem.c", "tests/nxdn/nxdn_channel.c", *LIB,
         RF / "dmr/ambe/ambeFec.c", "-lm",
         "-o", OUT / "test_nxdn_modem"])
    run([OUT / "test_nxdn_modem"])
    run(["clang", *CFLAGS, "tests/nxdn/test_nxdn_link.c", *LIB, "-lm", "-o", OUT / "test_nxdn_link"])
    run([OUT / "test_nxdn_link"])
    # The radio channel (integration/radiofoundry/nxdn/radio_nxdn.c) with RadioFoundry's voice bridge and a fake board
    inc = ["-Iintegration/radiofoundry", "-Iintegration/radiofoundry/nxdn", "-I" + str(RF), "-I" + str(RF.parent / "hal"),
           "-I" + str(RF / "native_modem"), "-I" + str(RF.parent / "drivers")]
    run(["clang", *[f for f in CFLAGS if f != "-std=c11"], "-std=gnu11", "-Wno-unused-parameter", "-DCONFIG_NXDN=1",
         "-DCONFIG_DMR=1", *inc, "tests/nxdn/test_radio_nxdn.c", "integration/radiofoundry/nxdn/radio_nxdn.c", *LIB,
         RF / "dmr/ambe/ambeVoice.c", RF / "dmr/ambe/ambeFec.c", "-lm", "-o", OUT / "test_radio_nxdn"])
    run([OUT / "test_radio_nxdn"])
    if not (MMDVM / "NXDNSACCH.cpp").exists():
        print("SKIP MMDVMHost oracle: research/protocol/MMDVMHost missing (tools/build_validators.sh)")
        return 0
    objs = []
    (OUT / "mmdvm").mkdir(exist_ok=True)
    # The oracle is compiled without sanitizers: its FACCH1 encoder reads one entry past its puncture table
    for name in ("NXDNLICH", "NXDNSACCH", "NXDNFACCH1", "NXDNConvolution", "NXDNCRC", "NXDNAudio", "Golay24128"):
        obj = OUT / "mmdvm" / (name + ".o")
        run(["clang++", "-std=c++17", "-O1", "-g", "-w", "-I" + str(MMDVM), "-c", MMDVM / (name + ".cpp"), "-o", obj])
        objs.append(obj)
    obj = OUT / "mmdvm" / "utils_count.o"
    run(["clang++", "-std=c++17", "-O1", "-g", "-I" + str(MMDVM), "-c", "tests/nxdn/stubs/utils_count.cc", "-o", obj])
    objs.append(obj)
    cobjs = []
    for src in (*LIB, RF / "dmr/ambe/ambeFec.c"):
        o = OUT / (Path(src).stem + ".o")
        run(["clang", "-std=c11", "-O1", "-g", *SAN, "-I" + str(ROOT / "src/nxdn"), "-I" + str(RF), "-c", src, "-o", o])
        cobjs.append(o)
    run(["clang++", "-std=c++17", "-O1", "-g", "-Wall", *SAN, "-I" + str(MMDVM), "-I" + str(ROOT / "src/nxdn"), "-I" + str(RF),
         "tests/nxdn/test_nxdn_mmdvm.cc", *objs, *cobjs, "-o", OUT / "test_nxdn_mmdvm"])
    run([OUT / "test_nxdn_mmdvm"])
    run(["clang++", "-std=c++17", "-O1", "-g", *SAN, "-I" + str(MMDVM), "-I" + str(ROOT / "src/nxdn"),
         "tests/nxdn/test_nxdn_fec_compare.cc", *objs, *cobjs, "-o", OUT / "test_nxdn_fec_compare"])
    run([OUT / "test_nxdn_fec_compare"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
