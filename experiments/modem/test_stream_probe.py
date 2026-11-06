#!/usr/bin/env python3
"""Tests for preservation of native-buffer evidence, not radio validation."""

import hashlib
import unittest

from stream_probe import (air_from_rx_ram, apply_mapping, compare, pack, plan,
                          rx_ram_from_air, unpack)


def noise(n):
    return b"".join(hashlib.sha256(i.to_bytes(4, "big")).digest()
                    for i in range((n + 31) // 32))[:n]


class LayoutTests(unittest.TestCase):
    def test_voice_from_separate_fields(self):
        # Fields independently placed in manual Figure 70 order.
        cach = f"{0xEA12F1:024b}"
        sync = f"{0x5575F5FF77FF:048b}"
        first = ("10010110" * 14)[:108]
        second = ("00111011" * 14)[:108]
        raw = pack(cach + sync + first + second)
        air = pack(cach + first + sync + second)
        self.assertEqual(air_from_rx_ram(raw, 0), air)
        self.assertEqual(air_from_rx_ram(raw, 1), air)

    def test_data_from_separate_fields(self):
        cach = f"{0x4735AA:024b}"
        slot_first, slot_second = "1010010100", "0101001011"
        sync = f"{0xD5D7F77FD757:048b}"
        first = ("10010110" * 13)[:98]
        second = ("00111011" * 13)[:98]
        raw = pack(cach + slot_first + slot_second + sync + first + second)
        air = pack(cach + first + slot_first + sync + slot_second + second)
        self.assertEqual(air_from_rx_ram(raw, 2), air)

    def test_refuse_truncation_or_unknown_rc(self):
        with self.assertRaises(ValueError):
            air_from_rx_ram(bytes(33), 1)
        with self.assertRaises(ValueError):
            air_from_rx_ram(bytes(36), 3)

    def test_no_lost_bit_in_each_supported_layout(self):
        for kind in (0, 1, 2):
            for bit in range(288):
                air = pack("0" * bit + "1" + "0" * (287 - bit))
                self.assertEqual(air_from_rx_ram(rx_ram_from_air(air, kind), kind), air)


class EvidenceTests(unittest.TestCase):
    def test_sign_flip_is_sign_dibit_not_all_bits(self):
        self.assertEqual(apply_mapping(bytes([0x1B]), "rf-sign"), bytes([0xB1]))

    def test_exact_stream(self):
        data = noise(36 * 25)
        r = compare(data, data)
        self.assertTrue(r["exact_equal"])
        self.assertEqual(r["observed_offsets_bits"], [0])
        self.assertEqual(r["constant_offset_comparison"]["mismatch_count"], 0)

    def test_cach_insertion_changes_offset(self):
        data = noise(36 * 25)
        capture = data[:36 * 4] + bytes.fromhex("123456") + data[36 * 4:]
        r = compare(data, capture)
        self.assertFalse(r["exact_equal"])
        self.assertEqual(r["observed_offsets_bits"], [0, 24])
        self.assertEqual(r["boundary_offset_changes"][0]["net_inserted_bits"], 24)

    def test_dibit_loss_changes_offset(self):
        data = noise(36 * 25)
        bits = unpack(data)
        capture = pack(bits[:4 * 288] + bits[4 * 288 + 2:] + "00")
        r = compare(data, capture)
        self.assertEqual(r["observed_offsets_bits"], [-2, 0])
        self.assertEqual(r["boundary_offset_changes"][0]["net_inserted_bits"], -2)

    def test_duplicate_reordered_chunk_cannot_pass(self):
        data = noise(36 * 25)
        capture = data[:4 * 36] + data[3 * 36:4 * 36] + data[5 * 36:]
        r = compare(data, capture)
        self.assertFalse(r["exact_equal"])
        self.assertIn("captured-anchor-nonunique", [a["status"] for a in r["anchors"]])
        self.assertIn("not-found", [a["status"] for a in r["anchors"]])

    def test_plan_keeps_cai_and_explicit_tail(self):
        source = noise(549)
        packed, r = plan(source, 2)
        self.assertEqual(packed[:1098], source * 2)
        self.assertEqual(r["padding_bytes"], 18)
        self.assertEqual(len(packed) % 36, 0)
        self.assertEqual(r["entries"][0]["alternating_buffer_address"], "0x16000000")
        self.assertEqual(r["entries"][1]["alternating_buffer_address"], "0x16000024")
        self.assertFalse(r["hardware_buffer_ownership_verified"])


if __name__ == "__main__":
    unittest.main()
