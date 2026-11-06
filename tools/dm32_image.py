#!/usr/bin/env python3
"""Offline, hash-guarded DM32 container inspection and same-size patching.

No serial access. No image is promoted to a P25 release by this tool.
"""
import argparse
import hashlib
import json
from pathlib import Path


def digest(data):
    return hashlib.sha256(data).hexdigest()


def load_target(image, target):
    raw = image.read_bytes()
    spec = json.loads(target.read_text())
    if len(raw) != spec['image_size'] or digest(raw) != spec['image_sha256']:
        raise ValueError('Image size/hash does not match the selected target')
    if raw[:9] != b'BFUV32-V2' or raw[253:256] != b'END':
        raise ValueError('Unrecognized update container')
    if digest(raw[256:]) != spec['payload_sha256']:
        raise ValueError('Payload hash mismatch')
    return raw, spec


def apply_patches(raw, target, manifest):
    """Manifest offsets refer to original container file bytes, never chip addresses."""
    if manifest['input_sha256'] != digest(raw):
        raise ValueError('Patch input hash mismatch')
    if manifest['target_id'] != target['target_id']:
        raise ValueError('Patch target mismatch')
    result = bytearray(raw)
    used = set()
    patches = manifest['patches']
    if not patches:
        raise ValueError('Empty patch manifest')
    for patch in patches:
        offset = patch['file_offset']
        before = bytes.fromhex(patch['expected_hex'])
        after = bytes.fromhex(patch['replacement_hex'])
        if not isinstance(offset, int) or isinstance(offset, bool):
            raise ValueError('Offset must be an integer')
        if not before or len(before) != len(after):
            raise ValueError('Only nonempty, same-length replacements are supported')
        if offset < 256 or offset + len(before) > len(raw):
            raise ValueError('Patch extends outside the application payload')
        covered = set(range(offset, offset + len(before)))
        if used.intersection(covered):
            raise ValueError('Overlapping patches')
        used.update(covered)
        if raw[offset:offset + len(before)] != before:
            raise ValueError('Original bytes do not match patch guard')
        if before == after:
            raise ValueError('Patch does not change any bytes')
        result[offset:offset + len(after)] = after
    result = bytes(result)
    if digest(result) != manifest['output_sha256']:
        raise ValueError('Patched output hash mismatch')
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('command', choices=['inspect', 'extract', 'patch'])
    ap.add_argument('--image', type=Path, required=True)
    ap.add_argument('--target', type=Path, required=True)
    ap.add_argument('--manifest', type=Path)
    ap.add_argument('--out', type=Path)
    args = ap.parse_args()
    raw, spec = load_target(args.image, args.target)
    if args.command == 'inspect':
        print(json.dumps(spec, indent=2))
        return
    if args.out is None:
        ap.error('--out is required')
    if args.command == 'extract':
        result = raw[256:]
    else:
        if args.manifest is None:
            ap.error('--manifest is required for patch')
        result = apply_patches(raw, spec, json.loads(args.manifest.read_text()))
    # Exclusive creation preserves pristine inputs and previous artifacts.
    with args.out.open('xb') as f:
        f.write(result)
    print(json.dumps({'output': str(args.out), 'bytes': len(result),
                      'sha256': digest(result), 'hardware_verified': False,
                      'release_candidate': False}, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, OSError) as exc:
        raise SystemExit(str(exc))
