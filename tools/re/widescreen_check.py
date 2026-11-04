#!/usr/bin/env python3
"""Checks the runtime's --widescreen projection patch against the real game code (x86-64 Linux with gcc).

Runs the game's own projection routine (0x3ec310) natively, unpatched and with the cave bytes taken verbatim from
runtime/jstars_loader.c, and checks the matrix: 16:9 views get a wider horizontal field of view (Hor+) with the vertical one
unchanged; shadow (aspect 1) and 4:3 projections are untouched. Needs the prepared boot image from your own copy of the game
(the launcher writes it); nothing from the game is written to the repository.

    python3 tools/re/widescreen_check.py <prepared/boot-linked.bin>
"""
import pathlib, re, struct, subprocess, sys, tempfile

here = pathlib.Path(__file__).resolve().parent
root = here.parent.parent


def load_image(path):
    data = path.read_bytes()
    h = struct.unpack_from("<12Q", data, 8)
    size, ns, nr, ni, nm = h[0], h[2], h[3], h[4], h[11]
    off = 104 + nm * 56
    nb = struct.unpack_from("<Q", data, off)[0]
    off += 8 + nb * 24
    image_off = off + ns * 24 + ni * 128 + nr * 32
    return data[image_off:image_off + size]


def cave_bytes():
    src = (root / "runtime" / "jstars_loader.c").read_text()
    body = src[src.index("static const uint8_t code[0x58] = {"):]
    body = body[body.index("{") + 1:body.index("};")]
    code = [int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\b0\b", body)]
    if len(code) != 0x58:
        sys.exit(f"cave code: {len(code)} bytes, expected 0x58")
    return code


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    image = load_image(pathlib.Path(sys.argv[1]))
    with tempfile.TemporaryDirectory() as tmp:
        t = pathlib.Path(tmp)
        (t / "cave_bytes.h").write_text("static const unsigned char cave_code[0x58] = {" + ",".join(map(str, cave_bytes())) + "};\n")
        (t / "image.raw").write_bytes(image[:0x800000])
        subprocess.run(["gcc", "-O1", "-I", str(t), "-o", str(t / "check"), str(here / "widescreen_check.c"), "-lm"], check=True)
        return subprocess.run([str(t / "check"), str(t / "image.raw")]).returncode


if __name__ == "__main__":
    sys.exit(main())
