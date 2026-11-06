#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Capture local codec experiment evidence separately from pristine inputs."""
import datetime
import hashlib
import json
import platform
from pathlib import Path
import shutil
import wave

from build import HERE, ROOT


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    results = HERE / "results"
    results.mkdir(exist_ok=True)
    for build, output in (("release", "host"), ("ubsan", "sanitized")):
        for stem in ("synthetic", "speech", "speech-cai"):
            shutil.copyfile(HERE / "out" / build / (stem + ".json"),
                            results / (output + "-" + stem + ".json"))
    shutil.copyfile(ROOT / "research" / "codec" / "sources.json", results / "sources.json")
    for source, destination in (("build.json", "target-build.json"), ("stack.json", "target-stack.json"),
                                ("portability.patch", "portability.patch")):
        shutil.copyfile(HERE / "out" / "ck803s" / source, results / destination)
    comparisons = {}
    for name in ("libdm32_imbe.a", "codec-all.o"):
        a, b = HERE / "out" / "repro-a" / name, HERE / "out" / "repro-b" / name
        if a.read_bytes() != b.read_bytes():
            raise SystemExit(f"Reproducibility failure: {name}")
        comparisons[name] = {"sha256": digest(a), "bytes": a.stat().st_size, "bit_identical": True}
    (results / "reproducibility.json").write_text(json.dumps({
        "recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "method": "Two separate build.py --target --out directories, specific generated path maps and GNU ar rcsD",
        "directories": ["experiments/codec/out/repro-a", "experiments/codec/out/repro-b"],
        "files": comparisons,
        "scope": "codec objects/archive only; not a whole firmware build or a hardware test",
    }, indent=2) + "\n")
    diagnostics = {}
    for name in ("diagnostics.log", "speech-diagnostics.log", "pipeline-diagnostics.log"):
        path = HERE / "out" / "ubsan" / name
        diagnostics[name] = {"bytes": path.stat().st_size, "sha256": digest(path)}
        if path.stat().st_size:
            raise SystemExit(f"Sanitizer diagnostics need review: {path}")
    (results / "sanitizers.json").write_text(json.dumps({
        "recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "compiler_flags": "-O2 -fno-wrapv -fsanitize=address,undefined -fno-omit-frame-pointer",
        "exit_status": 0, "stderr": diagnostics,
        "scope": "observed test inputs only; no proof of exhaustive safety or CK803S execution",
    }, indent=2) + "\n")
    audio_records = {}
    for stem in ("speech.input", "speech.fixed", "speech.mbelib", "speech-cai.fixed", "speech-cai.mbelib"):
        raw = HERE / "out" / "release" / (stem + ".s16le")
        wav = raw.with_suffix(".wav")
        with wave.open(str(wav), "wb") as audio:
            audio.setnchannels(1); audio.setsampwidth(2); audio.setframerate(8000)
            audio.writeframes(raw.read_bytes())
        audio_records[str(wav.relative_to(ROOT))] = {"sha256": digest(wav), "bytes": wav.stat().st_size,
                                                    "sample_rate": 8000, "channels": 1,
                                                    "duration_seconds": raw.stat().st_size / 16000}
    for stem, suffix in (("speech", ".imbe88"), ("speech-cai", ".p25"), ("speech-cai", ".dibits")):
        file = HERE / "out" / "release" / (stem + suffix)
        audio_records[str(file.relative_to(ROOT))] = {"sha256": digest(file), "bytes": file.stat().st_size}
    (results / "speech-origin.json").write_text(json.dumps({
        "recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "platform": platform.platform(),
        "origin": "Locally generated macOS /usr/bin/say output from the original test text below; configured default system voice was used and was not pinned",
        "text": "This is a clear voice test of Project twenty five. The small portable radio must receive and transmit speech using its own microphone and speaker. Alpha bravo charlie. One two three four five. Testing conventional voice on a single channel.",
        "conversion": "ffmpeg -i speech.aiff -ar 8000 -ac 1 -f s16le speech.s16le",
        "license_record": "Generated local development audio, separate from GPL source. No redistribution license for the system voice/audio is asserted. Files stay in ignored local out/; deterministic mathematical synthetic input is the redistributable primary regression input.",
        "quality_limit": "No formal perceptual speech-quality score, listening-panel result, microphone capture, speaker test, or radio/RF interoperability measurement was performed.",
        "files": audio_records,
    }, indent=2) + "\n")
    print(results)


if __name__ == "__main__":
    main()
