import importlib.util
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('dm32_image', Path(__file__).parents[1] / 'tools/dm32_image.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class ImageGuards(unittest.TestCase):
    def setUp(self):
        self.raw = b'X' * 300
        self.target = {'target_id': 'test'}
        self.manifest = {'input_sha256': m.digest(self.raw), 'target_id': 'test',
                         'patches': [{'file_offset': 260, 'expected_hex': '5858', 'replacement_hex': '1234'}],
                         'output_sha256': m.digest(self.raw[:260] + bytes.fromhex('1234') + self.raw[262:])}

    def test_expected_result(self):
        self.assertEqual(m.apply_patches(self.raw, self.target, self.manifest)[260:262], b'\x12\x34')

    def test_bad_original(self):
        self.manifest['patches'][0]['expected_hex'] = '5959'
        with self.assertRaisesRegex(ValueError, 'Original bytes'):
            m.apply_patches(self.raw, self.target, self.manifest)

    def test_header_and_bounds(self):
        for offset in [-1, 0, 255, 299, 301]:
            self.manifest['patches'][0]['file_offset'] = offset
            with self.assertRaisesRegex(ValueError, 'outside'):
                m.apply_patches(self.raw, self.target, self.manifest)

    def test_overlap(self):
        self.manifest['patches'] *= 2
        with self.assertRaisesRegex(ValueError, 'Overlapping'):
            m.apply_patches(self.raw, self.target, self.manifest)

    def test_incompatible_input(self):
        with self.assertRaisesRegex(ValueError, 'input hash'):
            m.apply_patches(b'Y' * 300, self.target, self.manifest)

    def test_wrong_target(self):
        with self.assertRaisesRegex(ValueError, 'target mismatch'):
            m.apply_patches(self.raw, {'target_id': 'other-family'}, self.manifest)

    def test_wrong_output(self):
        self.manifest['output_sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'output hash'):
            m.apply_patches(self.raw, self.target, self.manifest)

    def test_length_change(self):
        self.manifest['patches'][0]['replacement_hex'] = '12'
        with self.assertRaisesRegex(ValueError, 'same-length'):
            m.apply_patches(self.raw, self.target, self.manifest)


if __name__ == '__main__':
    unittest.main()
