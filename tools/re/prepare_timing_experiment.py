"""Prepare an isolated, opt-in CUSA01593 timing experiment; never edit the input.

This is a diagnostic hypothesis, not a validated FPS fix. After a chosen guest
counter threshold, request mode 1 through the game's own presentation function
instead of overriding only the host flip rate. The returned step then follows
the selected mode. Keep generated boot files private.
"""
import argparse
import hashlib
import pathlib
import struct

EBOOT_SHA256 = 'd43b7b5db10b5808c06691b27ef0ce1b5d27da6b1e07d811db5bea125ceaed54'
BOOT_SHA256 = '8d356ebed54bdc6f985a9c88bbf48af370b6941aa9fe6225bf374d96411ef652'
SITE, RESUME, CAVE, COUNTER = 0x433e6a, 0x433e73, 0x730100, 0x81f374
EXPECTED = bytes.fromhex('4489feff90f0000000')
RATE_SITE = 0x1e01b1
RATE_EXPECTED = bytes.fromhex('bf1e000000e875412200')


def trampoline(threshold):
    if not 0 <= threshold <= 0x7fffffff:
        raise ValueError('threshold must fit a nonnegative signed 32-bit counter')
    code = bytearray(bytes.fromhex('4489fe'))  # original mov esi,r15d
    code += b'\x81\x3d' + struct.pack('<iI', COUNTER - (CAVE + 13), threshold)
    code += b'\x72\x05'  # jb: preserve the original mode before threshold
    code += b'\xbe\x01\x00\x00\x00'  # mov esi,1
    code += bytes.fromhex('ff90f0000000')  # original virtual call
    code += b'\xe9' + struct.pack('<i', RESUME - (CAVE + len(code) + 5))
    return code


def prepare(data, threshold, engine_rate=30):
    if engine_rate not in (30, 60):
        raise ValueError('only the 30/60 diagnostic rates are supported')
    if data[:8] != b'JSTBOOT5' or len(data) < 104:
        raise ValueError('expected JSTBOOT5')
    h = struct.unpack_from('<12Q', data, 8)
    size, entry, ns, nr, ni = h[:5]
    nm = h[11]
    if not (1 <= ns <= 64 and 1 <= nm <= 8 and nr <= 1000000 and ni <= 100000):
        raise ValueError('invalid metadata limits')
    off = 104 + nm * 56
    nb = struct.unpack_from('<Q', data, off)[0]
    if nb > ni:
        raise ValueError('invalid binding count')
    off += 8 + nb * 24
    segs = [struct.unpack_from('<3Q', data, off + i * 24) for i in range(ns)]
    reloc_off = off + ns * 24 + ni * 128
    image_off = reloc_off + nr * 32
    if image_off + size != len(data) or segs[0] != (0, 7536868, 5):
        raise ValueError('unexpected image layout')
    code = trampoline(threshold)
    if CAVE < segs[0][1] or CAVE + len(code) > ((segs[0][1] + 16383) & ~16383):
        raise ValueError('trampoline must fit executable segment page padding')
    ranges = [(SITE, RESUME), (CAVE, CAVE+len(code))]
    if engine_rate != 30:
        ranges.append((RATE_SITE, RATE_SITE+len(RATE_EXPECTED)))
    for i in range(nr):
        target = struct.unpack_from('<Q', data, reloc_off + i * 32)[0]
        if any(target < end and target+8 > begin for begin,end in ranges):
            raise ValueError('relocation overlaps experiment')
    if data[image_off + SITE:image_off + RESUME] != EXPECTED:
        raise ValueError('original instructions do not match')
    if engine_rate != 30 and data[image_off+RATE_SITE:image_off+RATE_SITE+len(RATE_EXPECTED)] != RATE_EXPECTED:
        raise ValueError('engine rate initializer does not match')
    if any(data[image_off + CAVE:image_off + CAVE + len(code)]):
        raise ValueError('trampoline padding is not empty')
    result = bytearray(data)
    result[image_off + CAVE:image_off + CAVE + len(code)] = code
    result[image_off + SITE:image_off + RESUME] = b'\xe9' + struct.pack('<i', CAVE - SITE - 5) + b'\x90' * 4
    if engine_rate != 30:
        result[image_off+RATE_SITE+1:image_off+RATE_SITE+5] = struct.pack('<I',engine_rate)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--eboot', type=pathlib.Path, required=True)
    p.add_argument('--boot', type=pathlib.Path, required=True)
    p.add_argument('--out', type=pathlib.Path, required=True)
    p.add_argument('--after-ticks', type=int, default=16200)
    p.add_argument('--engine-rate', type=int, choices=(30,60), default=30)
    args = p.parse_args()
    if hashlib.sha256(args.eboot.read_bytes()).hexdigest() != EBOOT_SHA256:
        p.error('unsupported eboot identity')
    original = args.boot.read_bytes()
    if hashlib.sha256(original).hexdigest() != BOOT_SHA256:
        p.error('unsupported prepared image identity; do not patch another revision')
    result = prepare(original, args.after_ticks, args.engine_rate)
    with args.out.open('xb') as f:
        f.write(result)
    print('EXPERIMENT ONLY: guest mode 1 after', args.after_ticks, 'counter ticks')
    print('Engine rate initialization:', args.engine_rate)
    print('output sha256:', hashlib.sha256(result).hexdigest())


if __name__ == '__main__':
    main()
