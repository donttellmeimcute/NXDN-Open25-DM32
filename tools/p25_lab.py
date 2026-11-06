#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Query/run the derivative's offline-in-device P25 test. Never transmits RF.

Requires an explicit device port; no automatic device discovery or flashing.
This tool has not been run against hardware. The plain laboratory image must
already be running with the normal backup/recovery gates satisfied.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
UP = ROOT / "research/upstream/radiofoundry"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", required=True)
    ap.add_argument("--run", action="store_true")
    ap.add_argument("--timeout", type=float, default=30)
    args = ap.parse_args()
    pin = subprocess.check_output(["git", "-C", str(UP), "rev-parse", "HEAD"], text=True).strip()
    if pin != "dfbaf20c400c73e2ccf6edb80af7d1d255bee418":
        raise SystemExit("RadioFoundry protocol pin mismatch")
    spec = importlib.util.spec_from_file_location("rf_debug", UP / "tools/rfoundry_debug.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    with module.DebugLink(port=args.port) as radio:
        if args.run: radio.request(0x20, b"\x01", busy_wait=0)
        deadline = time.monotonic() + args.timeout
        while True:
            status, payload = radio.request(0x20, b"\x00")
            report = dict(line.split("=", 1) for line in payload.decode("ascii").splitlines() if "=" in line)
            if not args.run or report.get("p25.state") != "1": break
            if time.monotonic() >= deadline: raise SystemExit("P25 laboratory test timed out")
            time.sleep(0.2)
    print(json.dumps(report, indent=2))
    if args.run and (report.get("p25.ok") != "1" or report.get("p25.state") != "2"):
        raise SystemExit("On-device software test did not pass; no RF result asserted")
    if args.run and (report.get("p25.pcm_crc") != "4e158373" or
                     report.get("p25.air_bytes") != "549" or report.get("p25.pcm_blocks") != "18"):
        raise SystemExit("On-device result differs from pinned host reference; investigate before RF integration")


if __name__ == "__main__": main()
