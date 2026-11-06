#!/usr/bin/env python3
"""Offline C7000 physical-buffer experiments; this program never opens a radio.

The manual's Figure 70 is an RX RAM layout, not on-air order.  Rearrangement
here is deliberately an explicit operation and remains a hardware hypothesis
until checked against an independently observed stream.  All packed files use
MSB-first bits; all JSON record indices and ranges are zero-based.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


CHUNK_BYTES = 36
CHUNK_BITS = CHUNK_BYTES * 8


def unpack(data: bytes) -> str:
    return "".join(f"{b:08b}" for b in data)


def pack(bits: str) -> bytes:
    if len(bits) % 8 or set(bits) - {"0", "1"}:
        raise ValueError("bit string must contain complete bytes")
    return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))


def air_from_rx_ram(raw: bytes, sync_type: int) -> bytes:
    """Invert Figure 70's continuous-mode permutation, retaining CACH bits.

    RX_TYPE_INFO[1:0] == 0/1: CACH24,SYNC48,INFO108,INFO108.
    RX_TYPE_INFO[1:0] == 2: CACH24,SLOT10,SLOT10,SYNC48,INFO98,INFO98.
    The mapping is from the visually inspected PDF, not extracted text order.
    RC (3) is not specified by that figure and is rejected.
    """
    if len(raw) != CHUNK_BYTES:
        raise ValueError("continuous physical RX record must contain 36 bytes")
    b = unpack(raw)
    if sync_type in (0, 1):
        return pack(b[:24] + b[72:180] + b[24:72] + b[180:])
    if sync_type == 2:
        return pack(b[:24] + b[92:190] + b[24:34] + b[44:92]
                    + b[34:44] + b[190:])
    raise ValueError("Figure 70 does not define RC sync_type=3")


def rx_ram_from_air(air: bytes, sync_type: int) -> bytes:
    """Inverse for synthetic layout tests, not an independent hardware oracle."""
    if len(air) != CHUNK_BYTES:
        raise ValueError("air chunk must contain 36 bytes")
    b = unpack(air)
    if sync_type in (0, 1):
        return pack(b[:24] + b[132:180] + b[24:132] + b[180:])
    if sync_type == 2:
        return pack(b[:24] + b[122:132] + b[180:190] + b[132:180]
                    + b[24:122] + b[190:])
    raise ValueError("Figure 70 does not define RC sync_type=3")


def apply_mapping(data: bytes, mapping: str) -> bytes:
    """Explicit hypotheses for byte/dibit and RF polarity disagreements."""
    if mapping == "identity":
        return data
    if mapping == "rf-sign":
        # Manual dibits 00=+1,01=+3,10=-1,11=-3.  RF inversion flips bit 1.
        return bytes(b ^ 0xAA for b in data)
    if mapping == "all-bits":
        return bytes(b ^ 0xFF for b in data)
    if mapping == "dibit-bit-swap":
        return bytes(((b & 0xAA) >> 1) | ((b & 0x55) << 1) for b in data)
    if mapping == "reverse-dibits":
        return bytes(((b & 3) << 6) | ((b & 12) << 2)
                     | ((b & 48) >> 2) | ((b & 192) >> 6) for b in data)
    if mapping == "reverse-byte-bits":
        return bytes(int(f"{b:08b}"[::-1], 2) for b in data)
    raise ValueError(f"unknown mapping: {mapping}")


MAPPINGS = ("identity", "rf-sign", "all-bits", "dibit-bit-swap",
            "reverse-dibits", "reverse-byte-bits")


def exact_runs(expected: str, captured: str, anchor_bits: int = 64) -> list[dict]:
    """Find unique exact anchors on 36-byte expected boundaries.

    Changes in (captured_bit - expected_bit) are direct evidence of insertion
    or deletion between anchors.  No correction, synthetic bits or dropped
    CACH are introduced.  Missing/nonunique anchors remain explicit failures.
    A capture may start partway through the expected stream.
    """
    out = []
    for pos in range(0, len(expected) - anchor_bits + 1, CHUNK_BITS):
        anchor = expected[pos:pos + anchor_bits]
        own_first = expected.find(anchor)
        own_second = expected.find(anchor, own_first + 1)
        first = captured.find(anchor)
        second = captured.find(anchor, first + 1) if first >= 0 else -1
        if own_first != pos or own_second >= 0:
            out.append({"expected_bit": pos, "status": "expected-anchor-nonunique"})
        elif first < 0:
            out.append({"expected_bit": pos, "status": "not-found"})
        elif second >= 0:
            out.append({"expected_bit": pos, "status": "captured-anchor-nonunique"})
        else:
            out.append({"expected_bit": pos, "captured_bit": first,
                        "offset_bits": first - pos, "status": "unique"})
    return out


def compare(expected: bytes, captured: bytes, mapping: str = "identity") -> dict:
    observed = apply_mapping(captured, mapping)
    eb, cb = unpack(expected), unpack(observed)
    anchors = exact_runs(eb, cb)
    hits = [a for a in anchors if a["status"] == "unique"]
    offsets = sorted({a["offset_bits"] for a in hits})
    result = {
        "hardware_result": "unverified; offline file comparison only",
        "mapping": mapping,
        "expected_sha256": hashlib.sha256(expected).hexdigest(),
        "captured_original_sha256": hashlib.sha256(captured).hexdigest(),
        "expected_bits": len(eb), "captured_bits": len(cb),
        "exact_equal": expected == observed,
        "anchor_bits": 64, "anchor_stride_bits": CHUNK_BITS,
        "anchors": anchors, "observed_offsets_bits": offsets,
        "boundary_offset_changes": [],
    }
    for a, b in zip(hits, hits[1:]):
        delta = b["offset_bits"] - a["offset_bits"]
        if delta:
            result["boundary_offset_changes"].append({
                "after_expected_bit": a["expected_bit"],
                "by_expected_bit": b["expected_bit"],
                "net_inserted_bits": delta,
            })
    if len(offsets) == 1:
        offset = offsets[0]
        e_start, c_start = max(0, -offset), max(0, offset)
        count = min(len(eb) - e_start, len(cb) - c_start)
        bad = [i for i in range(max(0, count))
               if eb[e_start + i] != cb[c_start + i]]
        result["constant_offset_comparison"] = {
            "offset_bits": offset, "compared_bits": max(0, count),
            "mismatch_count": len(bad),
            "mismatch_expected_bits_first64": [e_start + i for i in bad[:64]],
            "expected_prefix_unobserved_bits": e_start,
            "expected_suffix_unobserved_bits": len(eb) - e_start - max(0, count),
            "captured_prefix_unmatched_bits": c_start,
            "captured_suffix_unmatched_bits": len(cb) - c_start - max(0, count),
        }
    return result


def plan(source: bytes, repeats: int) -> tuple[bytes, dict]:
    if not source or repeats < 1:
        raise ValueError("need nonempty source and positive repeats")
    stream = source * repeats
    # Preserve every source byte. Explicit zero tail completes the last chunk;
    # it is not a legal P25 terminator and must never be represented as one.
    pad = (-len(stream)) % CHUNK_BYTES
    packed = stream + bytes(pad)
    entries = []
    for i in range(len(packed) // CHUNK_BYTES):
        chunk = packed[i * CHUNK_BYTES:(i + 1) * CHUNK_BYTES]
        entries.append({"index": i, "nominal_start_us": i * 30000,
                        "alternating_buffer_address": hex(0x16000000 + (i % 2) * 36),
                        "data_hex": chunk.hex(),
                        "sha256": hashlib.sha256(chunk).hexdigest()})
    return packed, {
        "kind": "offline-buffer-plan; never an update image",
        "source_sha256": hashlib.sha256(source).hexdigest(),
        "source_bytes": len(source), "source_repeats": repeats,
        "symbol_rate": 4800, "bit_rate": 9600,
        "chunk_bytes": CHUNK_BYTES, "chunk_us": 30000,
        "padding_bytes": pad, "padding_is_cai": False,
        "hardware_buffer_ownership_verified": False,
        "write_deadlines": "Not specified: must measure safe inactive-buffer handoff first",
        "entries": entries,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("plan", help="segment packed reference bits into 36-byte chunks")
    p.add_argument("source", type=Path)
    p.add_argument("output_prefix", type=Path)
    p.add_argument("--repeat", type=int, default=1)
    c = sub.add_parser("compare", help="exact anchors and optional explicit mapping")
    c.add_argument("expected", type=Path)
    c.add_argument("captured", type=Path)
    c.add_argument("--mapping", choices=MAPPINGS, default="identity")
    c.add_argument("--output", type=Path)
    r = sub.add_parser("rx-air", help="apply manual Fig70 layout to JSONL 36-byte records")
    r.add_argument("capture_jsonl", type=Path)
    r.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.command == "plan":
        packed, report = plan(args.source.read_bytes(), args.repeat)
        args.output_prefix.parent.mkdir(parents=True, exist_ok=True)
        args.output_prefix.with_suffix(".rawbits").write_bytes(packed)
        args.output_prefix.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps({k: v for k, v in report.items() if k != "entries"}, indent=2))
    elif args.command == "compare":
        report = compare(args.expected.read_bytes(), args.captured.read_bytes(), args.mapping)
        payload = json.dumps(report, indent=2) + "\n"
        if args.output:
            args.output.write_text(payload)
        else:
            print(payload, end="")
    elif args.command == "rx-air":
        records, output, previous_seq = [], bytearray(), None
        for line_number, line in enumerate(args.capture_jsonl.read_text().splitlines(), 1):
            if not line.strip():
                continue
            row = json.loads(line)
            sequence = int(row["sequence"])
            if previous_seq is not None and sequence != previous_seq + 1:
                raise ValueError(f"line {line_number}: nonconsecutive sequence; preserve gap explicitly")
            previous_seq = sequence
            info = int(row["rx_type_info"], 0) if isinstance(row["rx_type_info"], str) else int(row["rx_type_info"])
            raw = bytes.fromhex(row["raw_hex"])
            output.extend(air_from_rx_ram(raw, info & 3))
            records.append({"sequence": sequence, "sync_type": info & 3})
        args.output.write_bytes(output)
        print(json.dumps({"layout": "manual-p296-fig70-continuous",
                          "hardware_layout_verified": False,
                          "output_sha256": hashlib.sha256(output).hexdigest(),
                          "records": records}, indent=2))


if __name__ == "__main__":
    main()
