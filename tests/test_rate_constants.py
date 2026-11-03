"""Synthetic tests for tools/re/rate_constants.py (no game data)."""
import struct
import sys
import pathlib
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools' / 're'))
import rate_constants as rc  # noqa: E402


def fake_image(relocs=(), size=0x600000):
    ns, nm, nb, ni = 1, 1, 0, 0
    header = b'JSTBOOT5' + struct.pack('<12Q', size, 0, ns, len(relocs), ni, 0, 0, 0, 0, 0, 0, nm)
    body = bytes(nm * 56) + struct.pack('<Q', nb) + bytes(ns * 24) + b''.join(struct.pack('<4Q', t, 0, 0, 0) for t in relocs)
    image = bytearray(size)
    for addr, v30, _ in rc.CONSTANTS:
        image[addr:addr + 4] = rc.f32(v30)
    return header + body + bytes(image), len(header) + len(body)


class RateConstants(unittest.TestCase):
    def test_60_rewrites_every_constant(self):
        data, off = fake_image()
        out = rc.apply(data, 60)
        for addr, _v, kind in rc.CONSTANTS:
            got = struct.unpack_from('<f', out, off + addr)[0]
            self.assertAlmostEqual(got, 1 / 60 if kind == 'step' else 60.0, places=6)
        self.assertEqual(len(out), len(data))

    def test_30_is_identity(self):
        data, _ = fake_image()
        self.assertEqual(bytes(rc.apply(data, 30)), data)

    def test_already_patched_is_refused(self):
        data, _ = fake_image()
        with self.assertRaises(ValueError):
            rc.apply(bytes(rc.apply(data, 60)), 60)

    def test_unsupported_rate(self):
        data, _ = fake_image()
        for rate in (120, 144, 0):
            with self.assertRaises(ValueError):
                rc.apply(data, rate)

    def test_relocation_overlap_refused(self):
        data, _ = fake_image(relocs=(rc.CONSTANTS[0][0] - 4,))
        with self.assertRaises(ValueError):
            rc.apply(data, 60)


if __name__ == '__main__':
    unittest.main()
