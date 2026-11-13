#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent checks of the P25 trunking signalling (src/p25t). No radio, no RF.

TSBK fields: outbound TSBKs built by src/p25t/tmsg.c (tests/p25t/tsbk_vectors.c) are parsed by OP25's own trunking
decoder, p25_system.decode_tsbk in research/protocol/op25/.../apps/tk_p25.py at the pinned commit (GNU Radio stubbed
out: the parser is plain Python), and every value OP25 reports is compared with the value put in.

TSDU frames: a control channel's outbound stream from src/p25t (tests/p25t/cc_air.c: TSDUs of one to three blocks,
back to back, with the trellis code, interleaving, CRC, NID and status symbols) is decoded by dsd-fme from a 48 kHz
discriminator recording with held symbol levels; every message dsd-fme reports is counted and its values compared.

    python3 tools/p25t_validate.py --check
"""
import argparse
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import types

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/p25t-host"
OP25_APPS = ROOT / "research/protocol/op25/op25/gr-op25_repeater/apps"
DSD = ROOT / "build/validators/dsd-fme/build/dsd-fme"
LEVEL = {1: 3, 0: 1, 2: -1, 3: -3}


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run(["clang", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-Isrc/p25", "-Isrc/p25t",
                    "tests/p25t/tsbk_vectors.c", "src/p25t/tmsg.c", "src/p25t/tsbk.c", "src/p25/p25.c",
                    "-o", str(OUT / "tsbk_vectors")], check=True, cwd=ROOT)
    subprocess.run(["clang", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-Isrc/p25", "-Isrc/p25t",
                    "tests/p25t/cc_air.c", "src/p25t/tmsg.c", "src/p25t/tsbk.c", "src/p25/p25.c",
                    "-o", str(OUT / "cc_air")], check=True, cwd=ROOT)


def write_wav(path, dibits, rate=48000):
    """Held symbol levels, 10 samples a symbol, +-1800 Hz outer deviation at 4 kHz full scale"""
    import struct
    pcm = [int(LEVEL[d] * 600 / 4000 * 32767) for d in dibits for _ in range(rate // 4800)]
    lead = [0] * (rate // 5)
    pcm = lead + pcm + lead
    data = struct.pack("<%dh" % len(pcm), *pcm)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def dsd_control_channel():
    """dsd-fme on the control channel stream: messages heard and their values"""
    dib = OUT / "cc.dibits"
    subprocess.run([str(OUT / "cc_air"), str(dib), "293"], check=True)
    wav = OUT / "cc_nrz.wav"
    write_wav(wav, dib.read_bytes())
    run = subprocess.run([str(DSD), "-o", "null", "-Z", "-i", wav.name, "-s", "48000"], cwd=OUT, capture_output=True,
                         text=True, timeout=300, stdin=subprocess.DEVNULL)
    text = re.sub(r"\x1b\[[0-9;]*m", "", run.stdout + run.stderr)
    count = lambda pat: len(re.findall(pat, text))
    heard = {
        "tsdu_frames": count(r"Sync: \+P25p1 .*TSBK"),
        "iden_up_vu": count(r"Identifier Update UHF/VHF\s*\n\s*Channel Identifier \[1\] BW \[5\] Transmit Offset \[2E10\]\s*\n"
                            r"\s*Channel Spacing \[064\] Base Frequency \[0A2510A2\] \[851006250\]"),
        "net_sts": count(r"Network Status Broadcast TSBK - Abbreviated\s*\n\s*WACN \[BEE00\] SYSID \[3A5\] NAC \[293\]"
                         r"\s*\n\s*Frequency \[851\.018750\] MHz"),
        "rfss_sts": count(r"LRA \[01\] SYSID \[3A5\] RFSS ID \[002\] SITE ID \[003\] CHAN \[1001\] SSC \[70\]"),
        "grant": count(r"Group Voice Channel Grant\s*\n\s*SVC \[02\] CHAN \[1010\] Group \[10101\] Source \[1234567\]"
                       r"\s*\n\s*Frequency \[851\.206250\] MHz"),
        "grant_update": count(r"Channel 1 \[1010\] Group 1 \[10101\]\[2775\]\s*\n\s*Frequency \[851\.206250\] MHz\s*\n"
                              r"\s*Channel 2 \[1012\] Group 2 \[300\]\[012C\]\s*\n\s*Frequency \[851\.231250\] MHz"),
        "u_reg_rsp": count(r"Unit Registration Response - SITE: 3A5 SRC_ID: 1234567 SRC: 1234567 REG_ACCEPT"),
    }
    sent = {"tsdu_frames": 18, "iden_up_vu": 6, "net_sts": 6, "rfss_sts": 6, "grant": 6, "grant_update": 6,
            "u_reg_rsp": 6}
    return {"sent": sent, "heard": heard, "match": heard == sent}


def op25_system():
    """OP25's p25_system with GNU Radio replaced by empty modules (decode_tsbk uses none of it)"""
    gr = types.ModuleType("gnuradio")
    gr.gr = types.ModuleType("gnuradio.gr")
    rep = types.ModuleType("gnuradio.op25_repeater")
    gr.op25_repeater = rep
    sys.modules.update({"gnuradio": gr, "gnuradio.gr": gr.gr, "gnuradio.op25_repeater": rep})
    sys.path.insert(0, str(OP25_APPS))
    import tk_p25  # noqa: E402
    err = io.StringIO()
    old = sys.stderr
    sys.stderr = err
    try:
        s = tk_p25.p25_system(10, {"sysname": "sim", "nac": "0x293", "control_channel_list": "851.0125"})
    finally:
        sys.stderr = old
    # the voice-frequency bookkeeping a grant triggers needs a receiver; only the parsed values matter here
    s.update_voice_frequency = lambda *a, **k: None
    return s


def parse_with_op25(system, hexstr):
    tsbk = int(hexstr[:20], 16)          # OP25 receives the 80 bits before the CRC
    err = io.StringIO()
    old = sys.stderr
    sys.stderr = err
    try:
        system.decode_tsbk(0, tsbk)
    finally:
        sys.stderr = old
    return err.getvalue()


HEX_FIELDS = {"syid", "wacn", "rfid", "lra", "ch1", "mfid"}   # OP25 prints these with %x


def nums(text):
    out = {}
    for k, v in re.findall(r"(\w+): (0x[0-9a-fA-F]+|-?[0-9a-fA-F]+)", text):
        try:
            out[k] = int(v, 16) if k in HEX_FIELDS else int(v, 0)
        except ValueError:
            pass
    return out


def compare(vec, text, system):
    """The values OP25 printed (debug 10), by opcode, against what was put in"""
    got = nums(text)
    op = vec["opcode"]
    if op == 0x00:
        exp = {"opts": vec["svc"], "ga": vec["group"], "sa": vec["source"]}
    elif op == 0x02:
        exp = {"ga1": vec["group1"], "ga2": vec["group2"]}
    elif op == 0x2C:
        exp = {"rv": vec["response"], "syid": vec["sysid"], "sid": vec["source_id"], "sa": vec["source_address"]}
    elif op == 0x28:
        exp = {"gav": vec["response"], "aga": vec["announce_group"], "ga": vec["group"], "ta": vec["target"]}
    elif op == 0x3A:
        exp = {"syid": vec["sysid"], "rfid": vec["rfss"], "stid": vec["site"], "A": vec["active"], "ch1": vec["channel"]}
    elif op == 0x3B:
        exp = {"wacn": vec["wacn"], "syid": vec["sysid"], "lra": vec["lra"], "ch1": vec["channel"]}
    elif op == 0x2F:
        exp = {"wacn": vec["wacn"], "syid": vec["sysid"], "sid": vec["target"]}
    elif op in (0x34, 0x3D, 0x33):
        band = system.freq_table.get(vec["id"])
        if not band:
            return False, {"error": "no band stored", "op25": text.strip()}
        exp = {}
        got = {}
        # OP25 stores the band (frequencies in Hz); compare what it would tune
        base = band.get("frequency")
        step = band.get("step")
        offset = band.get("offset")
        got = {"base_hz": base, "spacing_hz": step, "tx_offset_hz": offset}
        exp = {"base_hz": vec["base_hz"], "spacing_hz": vec["spacing_hz"], "tx_offset_hz": vec["tx_offset_hz"]}
        if op == 0x33:
            got["slots"] = band.get("tdma")
            exp["slots"] = vec["slots"]
    else:
        return False, {"error": "opcode not compared"}
    bad = {k: (v, got.get(k)) for k, v in exp.items() if got.get(k) != v}
    return not bad, {"expected": exp, "op25": {k: got.get(k) for k in exp}, "mismatch": bad}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true", help="exit 1 unless OP25 reads every field as written")
    args = ap.parse_args(argv)
    if not (OP25_APPS / "tk_p25.py").exists():
        raise SystemExit(f"OP25 missing at {OP25_APPS} (tools/fetch_protocol_sources.sh)")
    build()
    vectors = [json.loads(line) for line in subprocess.run([str(OUT / "tsbk_vectors")], check=True,
                                                             capture_output=True, text=True).stdout.splitlines()]
    system = op25_system()
    results = []
    ok = True
    for v in vectors:
        text = parse_with_op25(system, v["hex"])
        good, detail = compare(v, text, system)
        ok = ok and good
        results.append({"opcode": "0x%02x" % v["opcode"], "match": good, **detail})
    report = {"op25_tk_p25": {"tsbks": len(results), "matching": sum(r["match"] for r in results), "results": results}}
    if DSD.exists():
        report["dsd_fme_control_channel"] = dsd_control_channel()
        ok = ok and report["dsd_fme_control_channel"]["match"]
    else:
        report["dsd_fme_control_channel"] = f"dsd-fme not built at {DSD} (tools/build_validators.sh)"
    report["match"] = ok
    print(json.dumps(report, indent=2))
    return 0 if ok or not args.check else 1


if __name__ == "__main__":
    sys.exit(main())
