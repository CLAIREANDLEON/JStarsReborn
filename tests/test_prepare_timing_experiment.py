"""Synthetic image checks; no game data or game execution needed."""
import pathlib
import struct
import sys
import unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools/re'))
from prepare_timing_experiment import CAVE, COUNTER, EXPECTED, RESUME, SITE, RATE_SITE, RATE_EXPECTED, prepare, trampoline


def fixture(relocation=None):
    size = 0x820000
    header = b'JSTBOOT5' + struct.pack('<12Q', size, 0, 1, int(relocation is not None), 0, 1, 0, 0, 0, 0, 0, 1)
    header += bytes(56) + struct.pack('<Q3Q', 0, 0, 7536868, 5)
    if relocation is not None:
        header += struct.pack('<4Q', relocation, 0, 0, 0)
    image = bytearray(size)
    image[SITE:RESUME] = EXPECTED
    image[RATE_SITE:RATE_SITE+len(RATE_EXPECTED)] = RATE_EXPECTED
    return bytearray(header + image), len(header)


class TimingExperimentTests(unittest.TestCase):
    def test_preserves_original_and_only_changes_two_regions(self):
        data, offset = fixture()
        original = bytes(data)
        patched = prepare(data, 16200)
        self.assertEqual(bytes(data), original)
        changed = [i-offset for i,(a,b) in enumerate(zip(data,patched)) if a != b]
        self.assertTrue(changed)
        self.assertTrue(all(SITE <= i < RESUME or CAVE <= i < CAVE+31 for i in changed))
        self.assertEqual(len(patched), len(data))

    def test_branch_and_counter_targets(self):
        code = trampoline(16200)
        self.assertEqual(CAVE+13+struct.unpack_from('<i',code,5)[0], COUNTER)
        self.assertEqual(struct.unpack_from('<I',code,9)[0],16200)
        self.assertEqual(code[13:20], bytes.fromhex('7205be01000000'))
        self.assertEqual(code[20:26], EXPECTED[3:])
        self.assertEqual(CAVE+len(code)+struct.unpack_from('<i',code,len(code)-4)[0], RESUME)

    def test_rejects_mismatch_and_double_application(self):
        data, offset = fixture()
        patched = prepare(data,0)
        with self.assertRaises(ValueError): prepare(patched,0)
        data[offset+SITE] ^= 1
        with self.assertRaises(ValueError): prepare(data,0)

    def test_rejects_occupied_cave_and_overlapping_relocations(self):
        data, offset = fixture()
        data[offset+CAVE] = 1
        with self.assertRaises(ValueError): prepare(data,0)
        for target in (SITE-7,SITE,RESUME-1,CAVE-7,CAVE,CAVE+30):
            with self.subTest(target=target):
                data,_ = fixture(target)
                with self.assertRaises(ValueError): prepare(data,0)

    def test_rejects_threshold_overflow(self):
        for threshold in (-1, 0x80000000):
            with self.assertRaises(ValueError): trampoline(threshold)

    def test_engine_rate_patch_is_narrow_and_guarded(self):
        data,offset=fixture()
        mode_only=prepare(data,0)
        engine=prepare(data,0,60)
        self.assertEqual([i-offset for i,(a,b) in enumerate(zip(mode_only,engine)) if a!=b], [RATE_SITE+1])
        self.assertEqual(struct.unpack_from('<I',engine,offset+RATE_SITE+1)[0],60)
        data[offset+RATE_SITE+5]^=1
        with self.assertRaises(ValueError): prepare(data,0,60)
        data,_=fixture(RATE_SITE-7)
        with self.assertRaises(ValueError): prepare(data,0,60)
        with self.assertRaises(ValueError): prepare(data,0,120)


if __name__ == '__main__':
    unittest.main()
