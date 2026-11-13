#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent checks of P25 Phase 2 above the physical layer (src/p25p2). No radio, no RF.

tests/p25p2/p2_air.c writes a voice channel's outbound stream (dibits, one per byte: the OP25 / dsd-fme symbol
capture format): idle superframes, then a group call on each logical channel whose voice frames are AMBE+2 frames
built from known parameter bits by the radio's DMR FEC encoder, then the ends. Two decoders read it:

- dsd-fme (-f2, WACN / System ID / NAC given with -X): sync, ISCH, DUID, descrambling, SACCH / FACCH Reed-Solomon
  and CRC-12, the MAC PDUs (MAC_PTT, MAC_ACTIVE with Group Voice Channel User, MAC_END_PTT, MAC_HANGTIME,
  MAC_IDLE), ESS, and every AMBE+2 frame's 49 parameter bits after its own Golay decoding (mbelib). It checks
  MAC_END_PTT's CRC but does not print its addresses (it prints the call it had, then forgets it);
- OP25's Python TDMA decoder (apps/tdma: lfsr.py, isch.py, duid.py, vf.py, the loop of tdma-decode.py): its own
  scrambling sequence, ISCH and DUID codes, voice code word deinterleave and Golay decoding. Those modules are
  Python 2; the one Python 3 incompatibility in the path (an integer division in rs.gly23127GetSyn) is replaced here
  by the same computation with //, nothing else is changed.

Every value either decoder reports is compared with what went in.

    python3 tools/p25p2_validate.py --check
"""
import argparse
import contextlib
import io
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/p25p2-host"
DSD = ROOT / "build/validators/dsd-fme/build/dsd-fme"
TDMA = ROOT / "research/protocol/op25/op25/gr-op25_repeater/apps/tdma"
AMBE = ROOT / "build/radiofoundry-p25/core/dmr/ambe"
WACN, SYSID, NAC, FRAMES = 0xBEE00, 0x3A5, 0x293, 60


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    if not (AMBE / "ambeFec.c").exists():
        raise SystemExit(f"{AMBE}/ambeFec.c missing (python3 tools/build_radiofoundry.py)")
    subprocess.run(["clang", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-Isrc/p25p2", f"-I{AMBE}",
                    "tests/p25p2/p2_air.c", *sorted(str(p.relative_to(ROOT)) for p in (ROOT / "src/p25p2").glob("*.c")),
                    str(AMBE / "ambeFec.c"), "-o", str(OUT / "p2_air")], check=True, cwd=ROOT)


def generate():
    run = subprocess.run([str(OUT / "p2_air"), str(OUT / "p2.bin"), "%X" % WACN, "%X" % SYSID, "%X" % NAC, str(FRAMES)],
                         check=True, capture_output=True, text=True)
    lines = run.stdout.splitlines()
    calls = json.loads(lines[0])["calls"]
    for line in lines[1:]:
        d = json.loads(line)
        calls[d["channel"]]["params"] = d["params"]
        calls[d["channel"]]["padded"] = d["padded"]
    return calls


def silence_params():
    """The 49 parameter bits of the silence frame both ends pad with (decoded by the DMR FEC decoder)"""
    src = OUT / "silence.c"
    src.write_text('#include "ambeFec.h"\n#include <stdio.h>\nint main(void){static const unsigned char s[9]='
                   '{0xB9,0xE8,0x81,0x52,0x61,0x73,0x00,0x2A,0x6B};unsigned char b[49];ambeFecDecode(s,b,0);'
                   'for(int i=0;i<49;i++)putchar(48+b[i]);return 0;}\n')
    subprocess.run(["clang", "-std=c11", "-O1", f"-I{AMBE}", str(src), str(AMBE / "ambeFec.c"), "-o", str(OUT / "silence")],
                   check=True)
    return subprocess.run([str(OUT / "silence")], check=True, capture_output=True, text=True).stdout


def expected_frames(call, silence):
    return call["params"] + [silence] * call["padded"]


def dsd_fme(calls, silence):
    run = subprocess.run([str(DSD), "-f2", "-X", "%05X%03X%03X" % (WACN, SYSID, NAC), "-i", "p2.bin", "-o", "null",
                          "-Z"], cwd=OUT, capture_output=True, text=True, timeout=300, stdin=subprocess.DEVNULL)
    text = re.sub(r"\x1b\[[0-9;]*m", "", run.stdout + run.stderr)
    (OUT / "dsd-fme.txt").write_text(text)
    got = {"sync": len(re.findall(r"Sync: \+P25p2 WACN: %05X; SYS: %03X; NAC/CC: %03X;" % (WACN, SYSID, NAC), text))}
    frames = {0: [], 1: []}
    lch = None
    for line in text.splitlines():
        m = re.search(r"P25p2 LCH ([01])  (4V|2V)", line)
        if m:
            lch = int(m.group(1))
            continue
        m = re.match(r"\s*AMBE ([0-9A-F]{14}) err = \[(\d+)\] \[(\d+)\]", line)
        if m and lch is not None:
            bits = bin(int(m.group(1), 16) >> 7)[2:].zfill(49)
            frames[lch].append((bits, int(m.group(2)) + int(m.group(3))))
        elif "P25p2" in line:
            lch = None
    results = {"sync_lines": got["sync"]}
    ok = got["sync"] > 0
    for c, call in enumerate(calls):
        g, s = call["group"], call["source"]
        exp = expected_frames(call, silence)
        heard = [b for b, _ in frames[c]]
        r = {
            "mac_ptt": len(re.findall(r"LCH %d  MAC_PTT\s*\n VCH %d - TG %d SRC %d" % (c, c, g, s), text)),
            "mac_active_user": len(re.findall(r"SACCH  MAC_ACTIVE\s*\n VCH %d - TG: %d; SRC: %d;  Circuit Priority 4 Group "
                                              r"Voice - Abbreviated" % (c, g, s), text)),
            # dsd-fme accepts MAC_END_PTT only with a good CRC, then prints the call it had (from MAC_PTT / ACTIVE)
            # and forgets it: the first names our call, the rest show zeros
            "mac_end_ptt": len(re.findall(r"LCH %d  MAC_END_PTT" % c, text)),
            "end_closes_call": len(re.findall(r"LCH %d  MAC_END_PTT\s*\n VCH %d - TG %d SRC %d" % (c, c, g, s), text)),
            "ess_clear": len(re.findall(r"VCH %d - ESS_B 80000000000000000000000+ ERR = 00" % c, text)),
            "frames_sent": len(exp),
            "frames_heard": len(heard),
            "frames_matching": sum(a == b for a, b in zip(heard, exp)),
            "golay_corrections": sum(e for _, e in frames[c]),
        }
        r["match"] = (r["mac_ptt"] == 2 and r["mac_active_user"] >= 3 and r["mac_end_ptt"] == 2 and
                      r["end_closes_call"] == 1 and r["ess_clear"] >= 2
                      and heard == exp and r["golay_corrections"] == 0)
        ok = ok and r["match"]
        results["channel_%d" % c] = r
    results["match"] = ok
    return results


def op25_python(calls, silence):
    sys.path.insert(0, str(TDMA))
    import rs  # noqa: E402  (OP25)

    def gly23127GetSyn(pattern):   # rs.py's, with integer division
        aux = 0x400000
        while (pattern & 0xFFFFF800) != 0:
            while (aux & pattern) == 0:
                aux = aux >> 1
            pattern = pattern ^ (aux // 0x800 * 0xC75)
        return pattern
    rs.gly23127GetSyn = gly23127GetSyn
    import runpy
    buf = io.StringIO()
    argv = sys.argv
    sys.argv = ["tdma-decode.py", "-v", "-i", str(OUT / "p2.bin"), "-n", str(NAC), "-s", str(SYSID), "-w", str(WACN)]
    try:
        with contextlib.redirect_stdout(buf):
            runpy.run_path(str(TDMA / "tdma-decode.py"), run_name="__main__")
    finally:
        sys.argv = argv
    text = buf.getvalue()
    (OUT / "op25-tdma.txt").write_text(text)
    frames = {0: [], 1: []}
    slot = None
    types = {"4v": 0, "2v": 0, "sacch w": 0, "facch w": 0, "unknown": 0}
    isch = {"sync": 0, "info": 0, "unknown": 0}
    for line in text.splitlines():
        m = re.match(r"=+ superframe \d+ timeslot (\d+) =+", line)
        if m:
            j = int(m.group(1))
            slot = (11 - j) if j >= 10 else j % 2
            continue
        if line.startswith("sync isch"):
            isch["sync"] += 1
        elif line.startswith("channel "):
            isch["info"] += 1
        elif line.startswith("unknown isch"):
            isch["unknown"] += 1
        m = re.match(r"burst at \d+ type (.*)$", line)
        if m:
            t = m.group(1)
            types["unknown" if t.startswith("unknown") else t] = types.get(t, 0) + 1
            continue
        if re.match(r"^\d+(\t\d+){8}$", line) and slot is not None:
            b = [int(x) for x in line.split("\t")]
            u0 = ((b[0] & 0x78) << 5) | ((b[1] & 0x1e) << 3) | ((b[2] & 0x1e) >> 1)
            u1 = ((b[3] & 0x1fe) << 3) | ((b[4] & 0x78) >> 3)
            u2 = ((b[5] & 0x1e) << 6) | ((b[6] & 0xe) << 3) | (b[7] & 0xe) | ((b[8] & 0x4) >> 2)
            u3 = (((b[1] & 1) << 13) | ((b[2] & 1) << 12) | ((b[0] & 7) << 9) | ((b[3] & 1) << 8) | ((b[4] & 7) << 5)
                  | ((b[5] & 1) << 4) | ((b[6] & 1) << 3) | ((b[7] & 1) << 2) | (b[8] & 3))
            frames[slot].append("%012s%012s%011s%014s" % (bin(u0)[2:].zfill(12), bin(u1)[2:].zfill(12),
                                                          bin(u2)[2:].zfill(11), bin(u3)[2:].zfill(14)))
    results = {"burst_types": types, "isch": isch}
    ok = types["unknown"] == 0 and isch["unknown"] == 0 and isch["sync"] > 0 and isch["info"] > 0
    for c, call in enumerate(calls):
        exp = expected_frames(call, silence)
        r = {"frames_sent": len(exp), "frames_heard": len(frames[c]),
             "frames_matching": sum(a == b for a, b in zip(frames[c], exp))}
        r["match"] = frames[c] == exp
        ok = ok and r["match"]
        results["channel_%d" % c] = r
    results["match"] = ok
    return results


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true", help="exit 1 unless both decoders read everything as written")
    ap.add_argument("--json", type=Path, help="write the report here too")
    args = ap.parse_args(argv)
    build()
    calls = generate()
    silence = silence_params()
    report = {"stream": {"wacn": "%05X" % WACN, "sysid": "%03X" % SYSID, "nac": "%03X" % NAC,
                         "calls": [{k: c[k] for k in ("channel", "group", "source", "padded")} for c in calls],
                         "frames_per_call": FRAMES}}
    ok = True
    if DSD.exists():
        report["dsd_fme"] = dsd_fme(calls, silence)
        ok = ok and report["dsd_fme"]["match"]
    else:
        report["dsd_fme"] = f"dsd-fme not built at {DSD} (tools/build_validators.sh)"
        ok = False
    if (TDMA / "tdma-decode.py").exists():
        report["op25_tdma_python"] = op25_python(calls, silence)
        ok = ok and report["op25_tdma_python"]["match"]
    else:
        report["op25_tdma_python"] = f"OP25 missing at {TDMA} (tools/fetch_protocol_sources.sh)"
        ok = False
    report["match"] = ok
    text = json.dumps(report, indent=2)
    print(text)
    if args.json:
        args.json.write_text(text + "\n")
    return 0 if ok or not args.check else 1


if __name__ == "__main__":
    sys.exit(main())
