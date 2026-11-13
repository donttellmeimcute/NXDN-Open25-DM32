#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent check of the P25 transmit chain with dsd-fme.

The firmware's chain runs on the host (tests/validation/p25_tx_air.c): PCM ->
IMBE -> CAI -> the 36-byte native PHY blocks. This tool turns those dibits
into a 48 kHz C4FM discriminator recording (TIA-102.BAAA-B 8.2: dibit 01 +3
(+1800 Hz), 00 +1 (+600 Hz), 10 -1, 11 -3; raised-cosine Nyquist pulse,
alpha 0.2) and decodes it with dsd-fme, an implementation that shares no code
with ours. It reports what dsd-fme found: NAC, talkgroup, source, voice frames
and decode errors. No radio, no RF.

    python3 tools/p25_validate.py [--seconds 3] [--snr-db 30]
"""
import argparse
import json
import math
from pathlib import Path
import random
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/validation"
DSD = ROOT / "build/validators/dsd-fme/build/dsd-fme"
CODEC = ROOT / "experiments/codec/out/ubsan/libdm32_imbe.a"
sys.path.insert(0, str(ROOT))
LEVEL = {1: 1.0, 0: 1.0 / 3, 2: -1.0 / 3, 3: -1.0}  # dibit -> symbol / 3 (P25 and HR_C7000 mapping)


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    flags = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "src/p25"), "-I" + str(ROOT / "src/codec")]
    objs = []
    for src in ["tests/validation/p25_tx_air.c", "src/p25/p25.c", "src/p25/voice.c", "src/p25/phy_stream.c"]:
        obj = OUT / (Path(src).stem + ".o")
        subprocess.run(["clang", *flags, "-c", str(ROOT / src), "-o", str(obj)], check=True)
        objs.append(str(obj))
    if not CODEC.exists():
        subprocess.run(["python3", str(ROOT / "experiments/codec/build.py"), "--full-ubsan"], check=True, cwd=ROOT)
    subprocess.run(["clang++", *objs, str(CODEC), "-fsanitize=address,undefined", "-lm", "-o", str(OUT / "p25_tx_air")], check=True)


def rc_pulse(sps, span, alpha=0.2):
    taps = []
    for n in range(-span * sps, span * sps + 1):
        t = n / sps
        if t == 0:
            h = 1.0
        elif alpha and abs(abs(2 * alpha * t) - 1) < 1e-9:
            h = math.pi / 4 * math.sin(math.pi * t) / (math.pi * t)
        else:
            h = math.sin(math.pi * t) / (math.pi * t) * math.cos(math.pi * alpha * t) / (1 - (2 * alpha * t) ** 2)
        taps.append(h)
    return taps


def c4fm_taps(rate=48000, span=16, bins=2048):
    """TIA-102.BAAA-B 8.3 combined H(f)P(f) (raised cosine to 2880 Hz times the integrate-and-dump compensation), the
    response of tools/c4fm_reference.py, at this sample rate."""
    from tools.c4fm_reference import response, SYMBOL_RATE
    sps = rate // SYMBOL_RATE
    df = 2880 / bins
    w = [response(k * df) * (1 if k in (0, bins) else 2) for k in range(bins + 1)]
    cos = [[math.cos(2 * math.pi * k * df * n / rate) for k in range(bins + 1)] for n in range(span * sps + 1)]
    half = [sum(wk * c for wk, c in zip(w, row)) * df / SYMBOL_RATE for row in cos]
    return half[:0:-1] + half


def c4fm(dibits, rate=48000, baud=4800, deviation=1800.0, snr_db=None, seed=1, shaped=True):
    """Discriminator output in Hz per sample: the symbol train through the C4FM transmit filter (or a plain
    raised-cosine Nyquist pulse with shaped=False)."""
    sps = rate // baud
    span = 8
    taps = c4fm_taps(rate) if shaped else rc_pulse(sps, span)
    lead = [0.0] * (rate // 5)
    n = len(dibits) * sps + len(taps)
    sig = [0.0] * n
    for k, d in enumerate(dibits):
        a = LEVEL[d] * deviation
        base = k * sps
        for i, h in enumerate(taps):
            sig[base + i] += a * h
    out = lead + sig + lead
    if snr_db is not None:
        rng = random.Random(seed)
        p = sum(x * x for x in sig) / len(sig)
        sd = math.sqrt(p / 10 ** (snr_db / 10))
        out = [x + rng.gauss(0, sd) for x in out]
    return out


def write_wav(path, hz_samples, rate=48000, full_scale_hz=4000.0):
    pcm = [max(-32767, min(32767, int(round(x / full_scale_hz * 32767)))) for x in hz_samples]
    with open(path, "wb") as f:
        data = struct.pack("<%dh" % len(pcm), *pcm)
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def decode(wav):
    run = subprocess.run([str(DSD), "-f1", "-o", "null", "-Z", "-i", str(wav), "-s", "48000", "-w", str(wav.with_suffix(".decoded.wav"))],
                         capture_output=True, text=True, timeout=300, stdin=subprocess.DEVNULL)
    return re.sub(r"\x1b\[[0-9;]*m", "", run.stdout + run.stderr)


def wav_rms(path):
    data = Path(path).read_bytes()
    i = data.find(b"data")
    if i < 0:
        return 0.0
    n = struct.unpack("<I", data[i + 4:i + 8])[0] // 2
    pcm = struct.unpack("<%dh" % n, data[i + 8:i + 8 + 2 * n])
    return math.sqrt(sum(x * x for x in pcm) / max(n, 1))


def summarize(text):
    frames = re.findall(r"Sync: \+P25p1\s+NAC/CC: (\w+);\s+(\w+)", text)
    s = {"frames": len(frames)}
    for duid in ("HDU", "LDU1", "LDU2", "TDU", "TDULC"):
        s[duid.lower()] = sum(1 for _, d in frames if d == duid)
    s["nac"] = sorted(set(n.upper() for n, _ in frames))
    calls = re.findall(r"Group (\d+) Source (\d+)", text)
    s["talkgroups"] = sorted(set(int(g) for g, _ in calls))
    s["sources"] = sorted(set(int(x) for _, x in calls))
    for key, pat in (("audio_errors", r"Total audio errors: (\d+)"), ("header_errors", r"Total header errors: (\d+)"),
                     ("irrecoverable_header_errors", r"Total irrecoverable header errors: (\d+)")):
        m = re.search(pat, text)
        s[key] = int(m.group(1)) if m else None
    s["encrypted"] = bool(re.search(r"ALG ID: 0x(?!80)", text))
    return s


def waveform(dibits, kind, snr_db):
    if kind == "nrz":
        # symbol levels held for a whole symbol: the bit-exact view of what the modem is handed
        sig = [LEVEL[d] * 1800.0 for d in dibits for _ in range(10)]
        lead = [0.0] * 9600
        out = lead + sig + lead
        if snr_db is not None:
            rng = random.Random(1)
            p = sum(x * x for x in sig) / len(sig)
            sd = math.sqrt(p / 10 ** (snr_db / 10))
            out = [x + rng.gauss(0, sd) for x in out]
        return out
    return c4fm(dibits, snr_db=snr_db, shaped=kind == "c4fm")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--seconds", type=float, default=3.0)
    ap.add_argument("--nac", default="293")
    ap.add_argument("--talkgroup", type=int, default=10101)
    ap.add_argument("--source", type=int, default=1234567)
    ap.add_argument("--waveform", choices=("nrz", "rc", "c4fm"), default="nrz",
                    help="nrz: held symbol levels (bit-exact); rc: raised-cosine pulse; c4fm: TIA H(f)P(f)")
    ap.add_argument("--snr-db", type=float, default=None)
    ap.add_argument("--keep-log", action="store_true")
    ap.add_argument("--check", action="store_true", help="exit 1 unless dsd-fme hears every frame and our IDs")
    args = ap.parse_args(argv)
    if not DSD.exists():
        raise SystemExit(f"dsd-fme not built at {DSD} (docs/validation.md)")
    build()
    dib = OUT / "p25_call.dibits"
    sent_imbe = OUT / "p25_call.imbe"
    meta = json.loads(subprocess.run([str(OUT / "p25_tx_air"), str(dib), args.nac, str(args.talkgroup), str(args.source),
                                      str(args.seconds), str(sent_imbe)], check=True, capture_output=True, text=True).stdout)
    dibits = list(dib.read_bytes())
    wav = OUT / f"p25_call_{args.waveform}.wav"
    write_wav(wav, waveform(dibits, args.waveform, args.snr_db))
    text = decode(wav)
    if args.keep_log:
        wav.with_suffix(".dsd.log").write_text(text)
    heard = summarize(text)
    # The voice: each IMBE frame dsd-fme recovered (its own FEC and deinterleave) against what our encoder produced.
    # The silence frames that pad the last LDU follow the speech
    ours = sent_imbe.read_text().split()
    theirs = re.findall(r"IMBE ([0-9A-F]{22}) err = \[(\d+)\] \[(\d+)\]", text)
    heard["imbe_frames"] = len(theirs)
    heard["imbe_with_corrections"] = sum(1 for _, a, b in theirs if int(a) or int(b))
    n = min(len(ours), len(theirs))
    # Bit 87 (u7 LSB) carries the alternating pattern src/p25/p25.c writes per frame (i + LDU2) & 1, not the
    # vocoder's: the 87 vocoder bits are compared, the pattern on its own
    vocoder = lambda h: int(h, 16) >> 1
    heard["imbe_matching_ours"] = sum(1 for k in range(n) if vocoder(theirs[k][0]) == vocoder(ours[k]))
    heard["imbe_compared"] = n
    pattern = [(k % 9 + (k // 9) % 2) & 1 for k in range(len(theirs))]
    heard["u7_lsb_pattern_ok"] = all(int(h, 16) & 1 == p for (h, _, _), p in zip(theirs, pattern))
    report = {"transmitted": {**meta, "nac": args.nac.upper(), "talkgroup": args.talkgroup, "source": args.source},
              "waveform": args.waveform, "snr_db": args.snr_db, "dsd_fme": heard}
    ok = (heard["frames"] == meta["tx_frames"] and heard["nac"] == [args.nac.upper()] and
          heard["talkgroups"] == [args.talkgroup] and heard["sources"] == [args.source] and
          heard["irrecoverable_header_errors"] == 0 and not heard["encrypted"] and
          heard["imbe_compared"] == meta["imbe_frames"] and heard["imbe_matching_ours"] == heard["imbe_compared"] and
          heard["imbe_with_corrections"] == 0 and heard["u7_lsb_pattern_ok"])
    report["match"] = ok
    print(json.dumps(report, indent=2))
    return 0 if ok or not args.check else 1


if __name__ == "__main__":
    sys.exit(main())
