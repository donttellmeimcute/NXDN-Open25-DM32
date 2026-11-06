#!/usr/bin/env python3
"""Actual binary dispatcher reachability checks for the RAM-only reader patch."""
from pathlib import Path
import struct
import unittest

from make_readonly_helper import (ORIGINAL_SHA256, PATCHES, ROOT, dispatch_trace,
                                  patched_helper, sha)


class ReadOnlyHelperTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.original = (ROOT / 'research/originals/C7000_read_progmem-helper.bin').read_bytes()
        cls.patched = patched_helper(cls.original)

    def test_original_hash_and_only_two_bytes_differ(self):
        self.assertEqual(sha(self.original), ORIGINAL_SHA256)
        self.assertEqual([i for i, (a, b) in enumerate(zip(self.original, self.patched)) if a != b],
                         [0xb4d, 0xc5b])
        self.assertEqual(sha(self.patched), '8d316eff7b2be89745162da739cecc8334e2befcd278fa6748bff44cc6bfde42')

    def test_original_write_handlers_are_reachable_control(self):
        self.assertEqual(dispatch_trace(self.original, 0x18)['route'], 'program-write')
        self.assertEqual(dispatch_trace(self.original, 0x16)['route'], 'data-write')

    def test_every_command_routes_only_to_read_or_nack(self):
        for command in range(256):
            with self.subTest(command=hex(command)):
                route = dispatch_trace(self.patched, command)['route']
                self.assertEqual(route, {0x17: 'program-read', 0x15: 'data-read'}.get(command, 'nack'))

    def test_malformed_framing_never_reaches_read_or_write(self):
        # Covers all prefix/suffix byte pairs for both retained read commands;
        # command checksum byte is independently handled inside each read path.
        for command in (0x15, 0x17):
            for prefix in range(256):
                for suffix in range(256):
                    if prefix == 2 and suffix == 3:
                        continue
                    route = dispatch_trace(self.patched, command, prefix, suffix)['route']
                    if route != 'nack':
                        self.fail(f'{command:#x}/{prefix:#x}/{suffix:#x} reached{route}')

    def test_a_regression_restoring_branch_is_detectable(self):
        # Removing either unconditional skip makes that write command reachable.
        for patch, command, route in ((PATCHES[0], 0x18, 'program-write'),
                                      (PATCHES[1], 0x16, 'data-write')):
            regressed = bytearray(self.patched)
            off = patch['file_offset']
            regressed[off:off + 2] = bytes.fromhex(patch['expected_hex'])
            self.assertEqual(dispatch_trace(bytes(regressed), command)['route'], route)

    def test_input_mutation_and_truncation_refused(self):
        for where in (0, 0x100, 0xb4c, 0xc5a, len(self.original) - 1):
            changed = bytearray(self.original)
            changed[where] ^= 1
            with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                patched_helper(bytes(changed))
        with self.assertRaises(ValueError):
            patched_helper(self.original[:-1])

    def test_unrecognized_dispatch_opcode_refused(self):
        changed = bytearray(self.patched)
        struct.pack_into('<H', changed, 0xb48, 0)
        with self.assertRaisesRegex(ValueError, 'Unexpected LRW'):
            dispatch_trace(bytes(changed), 0x17)


if __name__ == '__main__':
    unittest.main()
