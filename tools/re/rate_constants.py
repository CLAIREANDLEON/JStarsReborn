"""Rate-dependent constants for the opt-in native frame-rate presets (CUSA01593 v01.02; experimental).

The engine steps most of its timing by its own rate (setter 0x404330, 300 units a second, dt = 1/rate:
docs/reverse-engineering/NATIVE_60_EXPERIMENT.md). A few routines instead use a literal 30 Hz step, so at an engine
rate of 60 they run twice as fast. Each of these literals is a private 4-byte constant read by one routine only (one
RIP-relative reference each, two for the 0x255280 pair; none is relocated), so a preset rewrites the constants, as
Bloodborne's "60 FPS++" presets do, and leaves code alone. At rate 30 nothing changes.

    0x585c08  1/30  playback clock 0x2f8d70: t += speed * (1/30) per update (live hit at 0x2fa15c, Codex 2026-11-03)
    0x5791ec  30.0  fade-in 0x255120:  progress += 1 / (duration * 30)
    0x5791f0  30.0  fade-out 0x255180: progress -= 1 / (duration * 30)
    0x575868  30.0  fade-in 0x1ee120 (same pattern, second class)
    0x57586c  30.0  fade-out 0x1ee180
    0x579200  30.0  fade 0x255280 (in and out)

Run AFTER prepare_timing_experiment.py --engine-rate 60 (which sets the engine rate and presentation mode). Generated
boot images are derived from the user's game: keep them private, never publish them.
"""
import argparse
import hashlib
import pathlib
import struct

CONSTANTS = [  # (image offset, value at 30 Hz, kind)
    (0x585C08, 1.0 / 30.0, 'step'),
    (0x5791EC, 30.0, 'rate'),
    (0x5791F0, 30.0, 'rate'),
    (0x575868, 30.0, 'rate'),
    (0x57586C, 30.0, 'rate'),
    (0x579200, 30.0, 'rate'),
]
RATES = (30, 60)


def image_layout(data):
    """(image offset in the file, set of relocation targets) for a JSTBOOT5 prepared image."""
    if data[:8] != b'JSTBOOT5' or len(data) < 104:
        raise ValueError('expected JSTBOOT5')
    h = struct.unpack_from('<12Q', data, 8)
    size, _entry, ns, nr, ni = h[:5]
    nm = h[11]
    if not (1 <= ns <= 64 and 1 <= nm <= 8 and nr <= 1000000 and ni <= 100000):
        raise ValueError('invalid metadata limits')
    off = 104 + nm * 56
    nb = struct.unpack_from('<Q', data, off)[0]
    if nb > ni:
        raise ValueError('invalid binding count')
    off += 8 + nb * 24
    reloc_off = off + ns * 24 + ni * 128
    image_off = reloc_off + nr * 32
    if image_off + size != len(data):
        raise ValueError('unexpected image layout')
    targets = {struct.unpack_from('<Q', data, reloc_off + i * 32)[0] for i in range(nr)}
    return image_off, targets


def f32(v):
    return struct.pack('<f', v)


def apply(data, rate):
    if rate not in RATES:
        raise ValueError('only 30 and 60 are supported (300 units a second must divide evenly; see NATIVE_60_EXPERIMENT.md)')
    image_off, relocs = image_layout(data)
    out = bytearray(data)
    for addr, v30, kind in CONSTANTS:
        if any(t <= addr + 3 and addr < t + 8 for t in relocs if addr - 8 < t < addr + 4):
            raise ValueError(f'relocation overlaps constant {addr:#x}')
        cur = bytes(out[image_off + addr:image_off + addr + 4])
        if cur != f32(v30):
            raise ValueError(f'constant {addr:#x} is not the original {v30!r} (already patched, or another game revision)')
        new = 1.0 / rate if kind == 'step' else float(rate)
        out[image_off + addr:image_off + addr + 4] = f32(new)
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--boot', type=pathlib.Path, required=True, help='image from prepare_timing_experiment.py --engine-rate 60')
    p.add_argument('--out', type=pathlib.Path, required=True)
    p.add_argument('--rate', type=int, choices=RATES, default=60)
    a = p.parse_args()
    result = apply(a.boot.read_bytes(), a.rate)
    with a.out.open('xb') as f:
        f.write(result)
    print('EXPERIMENT ONLY: rate constants set for', a.rate, 'Hz; output sha256:', hashlib.sha256(result).hexdigest())


if __name__ == '__main__':
    main()
