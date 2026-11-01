#!/usr/bin/env python3
"""Map the game's calls to a set of console imports, from the user's own eboot.

    tools/re/map_imports.py <game-folder> [--libs libSceNp,libSceNet] [--out docs/reverse-engineering/FUNCTION_MAP.md]

The game folder is only read. Nothing from it is copied into the output: the report contains addresses,
import names and instruction counts, never game code or data bytes.

Method
    1. Parse eboot.bin (SELF container -> ELF) with the same parser the preparation tools use.
    2. Every import is reached through a relocated pointer slot; the relocation table gives the slot address
       for each imported symbol (NID#library).
    3. Disassemble the executable segments and record every `call`/`jmp` whose RIP-relative memory operand
       resolves to one of those slots. That is a complete static list of call sites, not only the ones a
       recorded run happened to reach.
    4. Recover function starts from the targets of direct `call rel32` instructions, then attribute each call
       site to the nearest function start at or below it.

Confidence: a call site is evidence. A function start recovered from call targets is evidence. A *name* for a
function is an inference and is marked as such; this tool does not invent names.
"""
import argparse, collections, json, pathlib, sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'prepare'))
from link_modules import module  # noqa: E402

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_MEM, CS_GRP_CALL, CS_GRP_JUMP
except ImportError:
    sys.exit('capstone is required: pip install capstone')


def nid_names(repo):
    """NID -> readable name, from the table the runtime already ships (src/prepare/nid_names.inc)."""
    out = {}
    inc = repo / 'src' / 'prepare' / 'nid_names.inc'
    if not inc.exists():
        return out
    import re as _re
    for m in _re.finditer(r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\}', inc.read_text()):
        out[m.group(1)] = m.group(2)
    return out


def pretty(name, names):
    nid, lib = name.split('#', 1)
    return f'{names.get(nid, nid)} ({lib})' if nid in names else f'{nid}#{lib}'


def executable_segments(ph):
    return [p for p in ph if p['type'] == 1 and (p['flags'] & 1)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game', type=pathlib.Path)
    ap.add_argument('--libs', default='libSceNp,libSceNet,libSceSsl,libSceHttp,libSceRudp')
    ap.add_argument('--out', type=pathlib.Path)
    ap.add_argument('--json', type=pathlib.Path)
    args = ap.parse_args()
    prefixes = tuple(x.strip() for x in args.libs.split(',') if x.strip())

    names_by_nid = nid_names(pathlib.Path(__file__).resolve().parents[2])
    m = module(args.game / 'eboot.bin')
    elf, ph = m['elf'], m['ph']

    # symbol index -> NID#library, and the slots each one is relocated into
    names = {}
    for i, sym in enumerate(m['symbols']):
        ident = sym['identity']
        if ident:
            names[i] = f'{ident[0]}#{ident[1][0]}'
    slots = {}               # slot address -> import name
    for target, kind, symid, addend in m['relocs']:
        name = names.get(symid)
        if name and name.split('#', 1)[1].startswith(prefixes):
            slots[target] = name

    # The text segment holds data as well as code (it starts with the interpreter path), so a linear
    # disassembly from its first byte stops immediately. Instead the segment is scanned for the two encodings
    # that reach an import, and every candidate is verified by decoding it at that exact address:
    #     FF /2 with a RIP-relative operand   call qword ptr [rip + disp32]
    #     FF /4 with a RIP-relative operand   jmp  qword ptr [rip + disp32]   (tail call)
    # A hit counts only when the slot it resolves to is one of the relocated import slots, which is a set of a
    # few dozen addresses, so a byte sequence that merely looks like one of these instructions is rejected.
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    md_fast = Cs(CS_ARCH_X86, CS_MODE_64)
    call_targets = set()
    sites = collections.defaultdict(list)
    insn_count = 0
    segs = executable_segments(ph)
    for p in segs:
        code = elf[p['offset']:p['offset'] + p['filesz']]
        base = p['vaddr']
        lo, hi = base, base + p['filesz']
        # indirect calls and tail jumps through an import slot
        for i in range(len(code) - 6):
            if code[i] != 0xFF:
                continue
            modrm = code[i + 1]
            if modrm not in (0x15, 0x25):         # /2 and /4 with RIP-relative addressing
                continue
            disp = int.from_bytes(code[i + 2:i + 6], 'little', signed=True)
            addr = base + i
            slot = addr + 6 + disp
            name = slots.get(slot)
            if not name:
                continue
            decoded = next(md.disasm(code[i:i + 8], addr), None)
            if decoded is None or decoded.size != 6:
                continue
            insn_count += 1
            sites[name].append((addr, decoded.mnemonic))
        # direct calls: their targets are function starts
        for i in range(len(code) - 5):
            if code[i] != 0xE8:
                continue
            rel = int.from_bytes(code[i + 1:i + 5], 'little', signed=True)
            target = base + i + 5 + rel
            if lo <= target < hi:
                call_targets.add(target)

    # Each import is reached through a one-instruction stub (`jmp qword ptr [rip + slot]`); the game's own code
    # calls that stub with a direct `call`. So the sites found above are the stubs, and the callers of interest
    # are the direct calls that target them. Calls made through a function pointer or a virtual table are not
    # visible this way and are not counted.
    stub_of = {}
    for name, lst in sites.items():
        for addr, mnem in lst:
            if mnem == 'jmp':
                stub_of.setdefault(name, addr)
    callers = collections.defaultdict(list)   # import name -> [caller addresses]
    for p in segs:
        code = elf[p['offset']:p['offset'] + p['filesz']]
        base = p['vaddr']
        stub_names = {a: n for n, a in stub_of.items()}
        for i in range(len(code) - 5):
            if code[i] != 0xE8:
                continue
            rel = int.from_bytes(code[i + 1:i + 5], 'little', signed=True)
            target = base + i + 5 + rel
            name = stub_names.get(target)
            if name:
                callers[name].append(base + i)

    # A direct-call target is kept as a function start only when the bytes there decode as a plausible first
    # instruction of a function. This removes targets produced by byte sequences that merely contain 0xE8.
    PROLOGUE = ('push', 'sub', 'mov', 'lea', 'xor', 'test', 'cmp', 'endbr64', 'jmp', 'movzx', 'movsx', 'and', 'add')
    verified = set()
    for p in segs:
        code = elf[p['offset']:p['offset'] + p['filesz']]
        base = p['vaddr']
        for t in call_targets:
            if not (base <= t < base + p['filesz'] - 16):
                continue
            ins = next(md_fast.disasm(code[t - base:t - base + 16], t), None)
            if ins is not None and ins.mnemonic in PROLOGUE:
                verified.add(t)
    call_targets = verified

    starts = sorted(call_targets)

    def enclosing(addr):
        lo, hi = 0, len(starts)
        while lo < hi:
            mid = (lo + hi) // 2
            if starts[mid] <= addr:
                lo = mid + 1
            else:
                hi = mid
        return starts[lo - 1] if lo else None

    functions = collections.defaultdict(lambda: collections.defaultdict(int))
    for name, lst in callers.items():
        for addr in lst:
            fn = enclosing(addr)
            functions[fn][name] += 1

    report = {
        'eboot_sha256': m['sha256'],
        'verified_import_call_sites': insn_count,
        'function_starts_recovered': len(starts),
        'imports_matched': len(sites),
        'import_stubs': {k: hex(v) for k, v in sorted(stub_of.items())},
        'callers': {k: [hex(a) for a in v] for k, v in sorted(callers.items())},
        'functions': {hex(f): dict(sorted(d.items())) for f, d in sorted(functions.items(), key=lambda kv: -sum(kv[1].values()))},
    }
    if args.json:
        args.json.write_text(json.dumps(report, indent=1))

    out = []
    out.append('# Function map: the game code that uses the console network and NP libraries\n')
    out.append('Generated by `tools/re/map_imports.py` from the owner\'s own `eboot.bin`')
    out.append(f'(SHA-256 `{m["sha256"]}`, CUSA01593 version 01.02). The game folder is only read and no game')
    out.append('code or data is reproduced here: this file records addresses and counts.\n')
    out.append(f'* Verified call sites into an import slot: {insn_count:,} in {len(executable_segments(ph))} executable segment(s)')
    out.append(f'* Function starts recovered from direct call targets: {len(starts):,}')
    out.append(f'* Imports of `{args.libs}` with at least one static call site: {len(sites)}')
    out.append(f'* Import stubs found: {len(stub_of)}; direct calls from game code into those stubs: {sum(len(v) for v in callers.values()):,}\n')
    out.append('## Confidence\n')
    out.append('A stub and a direct call to it are **observed** in the executable. A function start is **recovered** from the targets of')
    out.append('direct `call` instructions, so a function nothing calls directly (a virtual method, a callback passed')
    out.append('by address, a tail-called helper) can be missing and its call sites then attribute to the function')
    out.append('above it. Names are **not** inferred: a function is identified by its address only, until behaviour')
    out.append('recorded at run time justifies a name.\n')
    out.append('## Functions by number of network/NP call sites\n')
    out.append('| Function (eboot.bin+) | Call sites | Imports it calls |')
    out.append('|---|---|---|')
    for fn, d in sorted(functions.items(), key=lambda kv: (-sum(kv[1].values()), kv[0] or 0))[:40]:
        total = sum(d.values())
        labels = sorted(pretty(k, names_by_nid) for k in d)
        libs = ', '.join(f'`{x}`' for x in labels)
        out.append(f'| `{hex(fn) if fn is not None else "unknown"}` | {total} | {libs} |')
    out.append('\n## Call sites by import\n')
    out.append('| Import | Stub | Callers | Calling functions (eboot.bin+) |')
    out.append('|---|---|---|---|')
    for name in sorted(sites):
        cl = callers.get(name, [])
        fns = sorted({enclosing(a) for a in cl})
        shown = ', '.join(f'`{hex(f)}`' for f in fns[:8] if f is not None)
        if len(fns) > 8:
            shown += f', … ({len(fns) - 8} more)'
        out.append(f'| `{pretty(name, names_by_nid)}` | `{hex(stub_of[name])}` | {len(cl)} | {shown or "—"} |')
    text = '\n'.join(out) + '\n'
    if args.out:
        args.out.write_text(text)
        print(f'wrote {args.out} ({len(text)} bytes)')
    else:
        print(text)


if __name__ == '__main__':
    main()
