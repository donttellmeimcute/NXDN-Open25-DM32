#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare a saved P25 TX trace with the bytes demodulated from an independent capture.

Input 1 is the JSON that `tools/p25_channel.py trace` prints: the firmware's
handoff log (time, queue depth and the 36 bytes handed to the modem per block).
Input 2 is the on-air byte stream recovered from an independent I/Q capture,
one 36-byte block per handoff, as hex lines or a raw binary file, already
aligned to the first handoff.

For each block the tool reports how many of the 12 head symbols (first three
bytes) match the intended block and how many match the previous one, the same
statistic as PROGRESS.md 2026-11-06 (96.8% / 94.9% matched the previous block).
A clean PHY_RDY handoff shows the head matching the intended block. The tool
never opens a serial port.
"""
import argparse
import json
from pathlib import Path
import sys

HEAD = 3


def symbols(data):
    for byte in data:
        for shift in (6, 4, 2, 0):
            yield (byte >> shift) & 3


def same_symbols(a, b):
    return sum(x == y for x, y in zip(symbols(a), symbols(b)))


def load_trace(path):
    trace = json.loads(Path(path).read_text())
    blocks = [bytes.fromhex(b['raw_hex']) for b in trace['blocks']]
    if any(len(b) != 36 for b in blocks):
        raise ValueError('trace blocks must be 36 bytes')
    return trace.get('summary', {}), blocks


def load_air(path):
    data = Path(path).read_bytes()
    try:
        text = data.decode('ascii')
        lines = [l.strip() for l in text.splitlines() if l.strip()]
        if lines and all(len(l) == 72 for l in lines):
            return [bytes.fromhex(l) for l in lines]
    except (UnicodeDecodeError, ValueError):
        pass
    if len(data) % 36:
        raise ValueError('binary air file must hold whole 36-byte blocks')
    return [data[i:i + 36] for i in range(0, len(data), 36)]


def compare(intended, air):
    n = min(len(intended), len(air))
    report = {'blocks': n, 'head_symbols': 0, 'head_match_intended': 0, 'head_match_previous': 0,
              'tail_bytes': 0, 'tail_match_intended': 0, 'blocks_exact': 0, 'blocks_exact_after_head_substitution': 0}
    for k in range(1, n):
        head = air[k][:HEAD]
        report['head_symbols'] += 4 * HEAD
        report['head_match_intended'] += same_symbols(head, intended[k][:HEAD])
        report['head_match_previous'] += same_symbols(head, intended[k - 1][:HEAD])
        report['tail_bytes'] += 36 - HEAD
        report['tail_match_intended'] += sum(a == b for a, b in zip(air[k][HEAD:], intended[k][HEAD:]))
        report['blocks_exact'] += air[k] == intended[k]
        report['blocks_exact_after_head_substitution'] += intended[k][:HEAD] + air[k][HEAD:] == intended[k]
    if report['head_symbols']:
        report['head_match_intended_pct'] = round(100.0 * report['head_match_intended'] / report['head_symbols'], 1)
        report['head_match_previous_pct'] = round(100.0 * report['head_match_previous'] / report['head_symbols'], 1)
    if report['head_match_previous'] > report['head_match_intended'] * 2:
        report['verdict'] = 'head-of-block lag: TX RAM written after the modem fetched the head'
    elif report['blocks_exact'] == max(n - 1, 0):
        report['verdict'] = 'every compared block exact'
    else:
        report['verdict'] = 'errors not explained by head lag (check alignment and the capture)'
    return report


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('trace', help='JSON from tools/p25_channel.py trace')
    ap.add_argument('air', help='aligned on-air blocks: hex lines (72 digits) or binary')
    args = ap.parse_args(argv)
    summary, intended = load_trace(args.trace)
    report = compare(intended, load_air(args.air))
    report['trace_version'] = summary.get('version')
    print(json.dumps(report, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
