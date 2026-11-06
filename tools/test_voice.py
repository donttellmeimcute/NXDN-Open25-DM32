#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real IMBE + CAI + conventional lifecycle + native PHY adapter, host only."""
from pathlib import Path
import importlib.util
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/voice-host"
CODEC = ROOT / "experiments/codec/out/ubsan"


def run(args):
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location("cai_oracle", ROOT / "tests/protocol/test_p25.py")
    oracle = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(oracle)
    ref = oracle.reference()
    fixtures = {
        "encrypted_hdu": oracle.make_reference(ref, "construct_hdu", 0x293, 0, 0, 0, 0x84, 1, 42),
        "foreign_hdu": oracle.make_reference(ref, "construct_hdu", 0x293, 0, 0, 2, 0x80, 0, 42),
        "encrypted_ldu2": oracle.make_reference(ref, "construct_ldu2", 0x293, 0, 0, 0, 0, 0x84, 1),
    }
    (OUT / "reference_frames.h").write_text("/* Generated from pinned independent p25craft oracle. */\n" + "\n".join(
        f"static const unsigned char {name}[] = {{" + ",".join(str(x) for x in data) + "};"
        for name, data in fixtures.items()))
    run(["python3", "experiments/codec/build.py", "--full-ubsan"])
    flags = ["-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
             "-fno-omit-frame-pointer", "-Isrc/p25", "-Isrc/codec", "-I" + str(OUT)]
    objects = []
    for name in ("p25", "voice", "phy_stream", "selftest"):
        obj = OUT / (name + ".o")
        run(["clang", "-std=c11", *flags, "-c", ROOT / "src/p25" / (name + ".c"), "-o", obj])
        objects.append(obj)
    run(["clang", "-std=c11", *flags, "-c", "tests/test_voice.c", "-o", OUT / "test_voice.o"])
    run(["clang++", *flags, OUT / "test_voice.o", *objects, CODEC / "libdm32_imbe.a", "-o", OUT / "test_voice"])
    run([OUT / "test_voice"])


if __name__ == "__main__":
    main()
