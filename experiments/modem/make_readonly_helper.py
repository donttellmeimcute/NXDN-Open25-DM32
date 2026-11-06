#!/usr/bin/env python3
"""Guarded RAM-only dispatcher patch removing reachable flash write commands.

This is a patch to the pinned community startup RAM helper, not stock flash or
an OEM update. It retains the existing read routines and their 24-bit SPI
addresses. It is still research code requiring a target hardware test.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
ORIGINAL_SHA256 = "59de726d5a50cf17573d35e98d183a184d3cc5da81ca30e198fb061cd3ee0f46"
LOAD = 0x10000
ENTRY = 0x10974
PATCHES = (
    {"file_offset": 0xb4c, "runtime_address": 0x10b4c,
     "expected_hex": "4508", "replacement_hex": "4504",
     "target": 0x10bd6, "reason": "Always skip program-flash write command0x18"},
    {"file_offset": 0xc5a, "runtime_address": 0x10c5a,
     "expected_hex": "3708", "replacement_hex": "3704",
     "target": 0x10cc8, "reason": "Always skip data-flash write command0x16"},
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def patched_helper(original: bytes) -> bytes:
    if len(original) != 3840 or sha(original) != ORIGINAL_SHA256:
        raise ValueError("Refuse helper patch: original size/hash mismatch")
    result = bytearray(original)
    for patch in PATCHES:
        off = patch["file_offset"]
        before, after = bytes.fromhex(patch["expected_hex"]), bytes.fromhex(patch["replacement_hex"])
        if original[off:off + len(before)] != before:
            raise ValueError("Refuse helper patch: original branch bytes mismatch")
        result[off:off + len(after)] = after
    return bytes(result)


def dispatch_trace(helper: bytes, command: int, prefix: int = 2, suffix: int = 3) -> dict:
    """Small bounded interpreter for actual pinned dispatcher instructions.

    This is not a C-SKY emulator or a proof of the whole binary. It executes
    the four literal loads, CMPNE, BT16 and BR16 on the dispatcher spine and
    records whether a read, write or NACK entry is reached. Stops before any
    command handler or peripheral instruction. The loaded literal and opcode
    bytes are checked against the disassembly, rather than simulating an
    abstract rewritten if/else chain.
    """
    if len(helper) != 3840 or not all(0 <= x <= 255 for x in (command, prefix, suffix)):
        raise ValueError("Invalid helper/header for dispatch trace")
    # Parser at0x10b3e..46 masks the checksum byte only. It does not checksum
    # the command byte, which is one reason to remove write command reachability.
    r3 = int.from_bytes(bytes((prefix, command, 0, suffix)), "little")
    r2, cond, pc, trace = 0, False, 0x10b48, []
    # Exact LRW16 opcode and literal-pool address pairs, checked against helper.
    loads = {0x10b48: (0x0259, 0x10de0), 0x10bd6: (0x0359, 0x10dec),
             0x10c56: (0x1348, 0x10df4), 0x10cc8: (0x124c, 0x10df8)}
    terminals = {0x10b4e: "program-write", 0x10bdc: "program-read",
                 0x10c5c: "data-write", 0x10cce: "data-read", 0x10b9e: "nack"}
    for _ in range(32):
        if pc in terminals:
            return {"command": command, "route": terminals[pc], "terminal": hex(pc),
                    "trace": [hex(p) for p in trace]}
        trace.append(pc)
        off = pc - LOAD
        if off < 0 or off + 2 > len(helper):
            raise ValueError("Dispatcher trace left helper")
        word = struct.unpack_from("<H", helper, off)[0]
        if pc in loads:
            opcode, literal = loads[pc]
            if word != opcode:
                raise ValueError(f"Unexpected LRW instruction at{pc:#x}")
            r2 = struct.unpack_from("<I", helper, literal - LOAD)[0]
            pc += 2
        elif word == 0x648e:  # cmpne r3,r2
            cond = r3 != r2
            pc += 2
        elif word & 0xfc00 in (0x0400, 0x0800):
            delta = word & 0x3ff
            if delta & 0x200:
                delta -= 0x400
            jump = word & 0xfc00 == 0x0400 or cond
            pc = pc + delta * 2 if jump else pc + 2
        else:
            raise ValueError(f"Unsupported dispatcher instruction{word:#x} at{pc:#x}")
    raise ValueError("Dispatcher did not terminate within bounded trace")


def verify_assembler(out: Path, tools: Path) -> None:
    """Require GNU assembler to reproduce both original and replacement bytes."""
    for mnemonic, field in (("bt", "expected_hex"), ("br", "replacement_hex")):
        source = out / f"verify-{mnemonic}.S"
        obj = source.with_suffix(".o")
        binary = source.with_suffix(".bin")
        asm = source.with_suffix(".asm")
        # Local labels at exact helper offsets eliminate linker relaxation and
        # retain the original16-bit offsets. VMA is adjusted for disassembly.
        source.write_text(".text\n.org 0xb4c\n" + mnemonic + " skip_program\n"
                          ".org 0xbd6\nskip_program: rts\n"
                          ".org 0xc5a\n" + mnemonic + " skip_data\n"
                          ".org 0xcc8\nskip_data: rts\n")
        subprocess.run([str(tools / "csky-elf-as"), "-mcpu=ck803", "-EL",
                        "-o", str(obj), str(source)], check=True)
        subprocess.run([str(tools / "csky-elf-objcopy"), "-O", "binary", "-j", ".text",
                        str(obj), str(binary)], check=True)
        decoded = subprocess.check_output([str(tools / "csky-elf-objdump"), "-d",
                                           "--adjust-vma=0x10000", str(obj)], text=True)
        asm.write_text(decoded)
        data = binary.read_bytes()
        for patch in PATCHES:
            off = patch["file_offset"]
            expected = patch[field]
            if data[off:off + 2].hex() != expected:
                raise ValueError("GNU assembler did not reproduce guarded branch bytes")
            pattern = rf"\b{patch['runtime_address']:x}:\s+[0-9a-f]+\s+{mnemonic}\s+(?:0x)?{patch['target']:x}\b"
            if re.search(pattern, decoded) is None:
                raise ValueError("GNU objdump did not confirm branch target")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", type=Path, default=ROOT / "research/originals/C7000_read_progmem-helper.bin")
    ap.add_argument("--out", type=Path, default=ROOT / "artifacts/modem/readonly-helper")
    ap.add_argument("--binutils", type=Path, default=ROOT / "toolchain/bin")
    args = ap.parse_args()
    original = args.source.read_bytes()
    patched = patched_helper(original)
    args.out.mkdir(parents=True, exist_ok=True)
    verify_assembler(args.out, args.binutils)
    traces = [dispatch_trace(patched, c) for c in range(256)]
    for row in traces:
        expected = {0x17: "program-read", 0x15: "data-read"}.get(row["command"], "nack")
        if row["route"] != expected:
            raise ValueError("Patched dispatcher reached unexpected handler")
    # Wrap for reliable ABIv2 disassembly; raw RAM bytes remain unchanged.
    dest = args.out / "helper.bin"
    dest.write_bytes(patched)
    elf = args.out / "helper.analysis.elf"
    subprocess.run([str(args.binutils / "csky-elf-objcopy"), "-I", "binary", "-O",
                    "elf32-csky-little", "-B", "csky:ck803", "--change-section-address",
                    ".data=0x10000", str(dest), str(elf)], check=True)
    wrapped = bytearray(elf.read_bytes())
    struct.pack_into("<I", wrapped, 36, 0x20000009)
    elf.write_bytes(wrapped)
    with (args.out / "helper.asm").open("w") as f:
        subprocess.run([str(args.binutils / "csky-elf-objdump"), "-D", str(elf)], stdout=f, check=True)
    manifest = {"kind": "RAM helper with flash-write dispatcher entries bypassed",
                "hardware_verified": False, "whole_binary_formally_verified": False,
                "source_sha256": ORIGINAL_SHA256, "output_sha256": sha(patched),
                "bytes": len(patched), "load_address": hex(LOAD), "entry": hex(ENTRY),
                "unchanged_read_protocol": "0205 <sum256mod256> 03 followed by256bytes",
                "SPI_address_bits": 24, "identified_chip_capacities": None,
                "patches": list(PATCHES),
                "verification": "GNU as+objdump reproduce both branch encodings; all256dispatcher command paths traced",
                "original_write_routes": [dispatch_trace(original, c) for c in (0x18, 0x16)],
                "patched_primary_routes": [traces[c] for c in (0x17, 0x15, 0x18, 0x16)]}
    (args.out / "helper.manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (args.out / "dispatch-traces.json").write_text(json.dumps(traces, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
