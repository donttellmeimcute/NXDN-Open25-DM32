#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent checks of the NXDN48 chains, on the host. No radio, no RF.

Transmit: the firmware's chain (tests/validation/nxdn_tx_air.c: AMBE+2 parameters -> AMBE FEC -> NXDN call frames
-> the 8 kHz software modulator -> the chip's FM modulator modeled as a 3.6 kHz interpolator) is written as a 48 kHz
discriminator recording and decoded by dsd-fme, which shares no code with ours. Compared: RAN, VCALL source and
group, TX_REL, and every AMBE+2 frame dsd-fme recovers against the parameters we sent.

Receive: the same frames, as dibits, are shaped by MMDVM's NXDN transmit filters (RRC 0.2 and its sinc filter, the
coefficients read from research/protocol/MMDVM/NXDNTX.cpp at the pinned commit) instead of ours, carried through the
FM channel model, and received by the firmware's chain (tests/validation/nxdn_rx_air.c: demodulator, call layer,
AMBE FEC). Compared: the call's identifiers and every AMBE+2 frame played.

    python3 tools/nxdn_validate.py --check
    python3 tools/nxdn_validate.py --cnr-db 12
"""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/validation"
DSD = ROOT / "build/validators/dsd-fme/build/dsd-fme"
MMDVM = ROOT / "research/protocol/MMDVM/NXDNTX.cpp"
RF = ROOT / "build/radiofoundry-p25/core"
LEVEL = {1: 3, 0: 1, 2: -1, 3: -3}  # dibit -> symbol (NXDN, as DMR and P25)


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    if not RF.exists():
        subprocess.run(["python3", "tools/build_radiofoundry.py", "--prepare-only"], check=True, cwd=ROOT)
    lib = ["src/nxdn/nxdn.c", "src/nxdn/nxdn_call.c", "src/nxdn/nxdn_modem.c", "tests/nxdn/nxdn_channel.c",
           str(RF / "dmr/ambe/ambeFec.c")]
    flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Isrc/nxdn", "-Itests/nxdn", "-I" + str(RF)]
    for name in ("nxdn_tx_air", "nxdn_rx_air"):
        subprocess.run(["clang", *flags, f"tests/validation/{name}.c", *lib, "-lm", "-o", str(OUT / name)],
                       check=True, cwd=ROOT)


def write_wav(path, pcm16, rate=48000):
    lead = b"\0\0" * (rate // 5)
    data = lead + pcm16 + lead
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def transmit(args, headers):
    stem = OUT / f"nxdn_tx_h{headers}"
    meta = json.loads(subprocess.run(
        [str(OUT / "nxdn_tx_air"), str(stem.with_suffix(".s16")), str(stem.with_suffix(".ambe")),
         str(stem.with_suffix(".dibits")), str(args.ran), str(args.source), str(args.group), str(args.seconds),
         str(headers)], check=True, capture_output=True, text=True).stdout)
    return stem, meta


def dsd_decode(stem, waveform="ours"):
    """ours: the modulator's output (.s16); nrz: each symbol's level held for its whole length, the bit-exact view of
    the stream (dsd-fme samples symbol centers with its own crude timing, and on shaped waveforms it misreads the
    header frames' LICH, MMDVM's shaping included)"""
    wav = stem.with_suffix(f".{waveform}.wav")
    if waveform == "nrz":
        dib = stem.with_suffix(".dibits").read_bytes()
        pcm = [int(LEVEL[d] * 350 / 4000 * 32767) for d in dib for _ in range(20)]
        write_wav(wav, struct.pack("<%dh" % len(pcm), *pcm))
    else:
        write_wav(wav, stem.with_suffix(".s16").read_bytes())
    run = subprocess.run([str(DSD), "-fi", "-o", "null", "-Z", "-i", wav.name, "-s", "48000"], cwd=OUT,
                         capture_output=True, text=True, timeout=300, stdin=subprocess.DEVNULL)
    return re.sub(r"\x1b\[[0-9;]*m", "", run.stdout + run.stderr)


def align(ours, theirs):
    """theirs is ours from some frame on (a decoder that joins late): the offset, or None"""
    if not theirs:
        return None
    for off in range(len(ours) - len(theirs) + 1):
        if ours[off] == theirs[0]:
            return off
    return None


def dsd_summary(text, ours):
    frames = re.findall(r"Sync: NXDN48\s+(\w+)\s+(Voice|Data)", text)
    calls = re.findall(r"Group Call - (Half Duplex|\s*Transmission Release)\s.*?Src=(\d+) - Dst/TG=(\d+)", text)
    theirs = re.findall(r"AMBE ([0-9A-F]{14}) err = \[(\w+)\] \[(\w+)\]", text)
    s = {
        "frames": len(frames),
        "ran": sorted(set(int(r) for r in re.findall(r"RAN (\d+)", text))),
        "vcall": sorted(set((int(a), int(b)) for k, a, b in calls if "Half" in k)),
        "tx_rel": sum(1 for k, _, _ in calls if "Release" in k) > 0,
        "header_vcall_facch1": bool(re.search(r"PF 1/1 IDLE\s*\n\s*SACCH NSF.*VCALL", text)),
        "ambe_frames": len(theirs),
        "ambe_with_corrections": sum(1 for _, a, b in theirs if int(a, 16) or int(b, 16)),
    }
    hexes = [h for h, _, _ in theirs]
    off = align(ours, hexes)
    s["ambe_first_frame"] = off
    s["ambe_matching_ours"] = 0 if off is None else sum(1 for k, h in enumerate(hexes) if ours[off + k] == h)
    for key, pat in (("audio_errors", r"Total audio errors: (\d+)"), ("header_errors", r"Total header errors: (\d+)")):
        m = re.search(pat, text)
        s[key] = int(m.group(1)) if m else None
    return s


def mmdvm_coefficients():
    """MMDVM's RRC (interpolating, L = 10) and sinc filters, as q15 integers"""
    src = MMDVM.read_text()

    def table(name):
        m = re.search(r"static q15_t %s\[\] = \{([^}]*)\}" % name, src)
        return [int(x) for x in m.group(1).replace("\n", " ").split(",") if x.strip()]
    return table("RRC_0_2_FILTER"), table("NXDN_SINC_FILTER")


def mmdvm_waveform(dibits):
    """Deviation in Hz at 48 kHz: MMDVM's transmit filters at 24 kHz (10 samples a symbol, as NXDNTX.cpp), the
    air interface's polarity, then 2x interpolation"""
    import numpy as np
    rrc, sinc = mmdvm_coefficients()
    sym = np.array([LEVEL[d] for d in dibits], dtype=float)
    up = np.zeros(len(sym) * 10)
    up[::10] = sym
    x = np.convolve(np.convolve(up, np.array(rrc, float)), np.array(sinc, float))
    # scale: a long run of +3 reaches +1050 Hz
    dc = sum(rrc) / 10 * sum(sinc)
    x = x / dc * 350.0
    # 24 -> 48 kHz: zero-stuff, windowed-sinc low-pass at 10 kHz
    y = np.zeros(len(x) * 2)
    y[::2] = x * 2
    n = np.arange(-64, 65)
    h = np.sinc(2 * 10000 / 48000 * n) * 2 * 10000 / 48000 * np.blackman(len(n))
    h /= h.sum() / 1.0
    y = np.convolve(y, h)
    lead = np.zeros(48000 // 10)
    return np.concatenate([lead, y, lead])


def receive(args, stem, ours, cnr):
    dev = mmdvm_waveform(list(stem.with_suffix(".dibits").read_bytes()))
    f64 = stem.with_suffix(".mmdvm.f64")
    dev.astype("<f8").tofile(f64)
    played = stem.with_suffix(f".rx{int(cnr)}.ambe")
    r = json.loads(subprocess.run(
        [str(OUT / "nxdn_rx_air"), str(f64), str(played), str(cnr), str(args.offset_hz), str(args.ran), str(args.group)],
        check=True, capture_output=True, text=True).stdout)
    got = played.read_text().split()
    off = align(ours, got)
    r["ambe_first_frame"] = off
    r["ambe_matching_ours"] = 0 if off is None else sum(1 for k, h in enumerate(got) if off + k < len(ours) and ours[off + k] == h)
    r["cnr_db"] = cnr
    return r


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--seconds", type=float, default=3.0)
    ap.add_argument("--ran", type=int, default=12)
    ap.add_argument("--source", type=int, default=4001)
    ap.add_argument("--group", type=int, default=200)
    ap.add_argument("--cnr-db", type=float, default=300.0, help="receive check: C/N in 12.5 kHz (300 = no noise)")
    ap.add_argument("--offset-hz", type=float, default=0.0)
    ap.add_argument("--check", action="store_true", help="exit 1 unless both checks match")
    args = ap.parse_args(argv)
    build()
    report = {"transmitted": {"ran": args.ran, "source": args.source, "group": args.group}}
    ok = True
    # Transmit. dsd-fme confirms its sync over two frame sync words before it decodes a frame and settles its levels
    # over the next ones: it joins one to three frames into a transmission, so it can only show a header frame that
    # has others before it. The bit-exact run (nrz) sends three headers; the firmware's waveform run sends one, as the
    # firmware does
    if DSD.exists():
        for waveform, headers in (("nrz", 3), ("ours", 1)):
            stem, meta = transmit(args, headers)
            ours = stem.with_suffix(".ambe").read_text().split()
            heard = dsd_summary(dsd_decode(stem, waveform), ours)
            first = heard["ambe_first_frame"]
            heard["joined_after_frames"] = None if first is None else headers + first // 4
            report[f"dsd_fme_{waveform}"] = {"waveform": waveform, "sent": meta, "heard": heard}
            good = (heard["ran"] == [args.ran] and heard["vcall"] == [(args.source, args.group)] and heard["tx_rel"]
                    and heard["ambe_matching_ours"] == heard["ambe_frames"] and heard["ambe_with_corrections"] == 0
                    and first is not None and first + heard["ambe_frames"] == meta["ambe_frames"])
            if waveform == "nrz":
                good = good and heard["header_vcall_facch1"] and first == 0
            else:
                good = good and heard["joined_after_frames"] <= 4
            ok = ok and good
    else:
        report["dsd_fme"] = f"not built at {DSD} (tools/build_validators.sh)"
    if MMDVM.exists():
        stem, meta = transmit(args, 1)
        ours = stem.with_suffix(".ambe").read_text().split()
        rx = receive(args, stem, ours, args.cnr_db)
        report["mmdvm_waveform_to_our_receiver"] = {"sent": meta, "heard": rx}
        # Without noise every frame must play bit-exact; with noise the AMBE FEC may leave a frame wrong
        clean = args.cnr_db >= 100
        ok = ok and (rx["calls"] == 1 and rx["releases"] == 1 and rx["source"] == args.source
                     and rx["destination"] == args.group and rx["ran"] == args.ran
                     and (rx["ambe_matching_ours"] == rx["slots_played"] == meta["ambe_frames"] and not rx["late_entries"]
                          if clean else rx["ambe_matching_ours"] >= 0.95 * meta["ambe_frames"]))
    else:
        report["mmdvm"] = f"missing {MMDVM} (tools/fetch_protocol_sources.sh)"
    report["match"] = ok
    print(json.dumps(report, indent=2))
    return 0 if ok or not args.check else 1


if __name__ == "__main__":
    sys.exit(main())
