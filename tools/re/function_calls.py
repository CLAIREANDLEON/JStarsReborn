#!/usr/bin/env python3
"""List, in order, the console imports and helper functions a single game function calls.

    tools/re/function_calls.py <game-folder> 0x2cc710 [0x2bdd40 ...]

Reads the owner's own eboot.bin and prints the call sequence of each function: the import names in the order
the code reaches them, plus the addresses of the helper functions it calls. No game code or data is printed,
and nothing is written into the repository by this tool; it is an instrument for building the function map.

The end of a function is found by following the straight-line flow and stopping at the first `ret` that is not
inside a known forward jump target, which is a heuristic: a function with an unusual layout can be truncated or
over-read. Treat the output as evidence about the order of calls, not as a decompilation.
"""
import argparse, pathlib, sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'prepare'))
from link_modules import module  # noqa: E402
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_MEM, CS_OP_IMM  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from map_imports import nid_names, pretty  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game', type=pathlib.Path)
    ap.add_argument('addresses', nargs='+')
    ap.add_argument('--max-insns', type=int, default=4000)
    args = ap.parse_args()

    repo = pathlib.Path(__file__).resolve().parents[2]
    names_by_nid = nid_names(repo)
    m = module(args.game / 'eboot.bin')
    elf, ph = m['elf'], m['ph']
    seg = next(p for p in ph if p['type'] == 1 and (p['flags'] & 1))
    code = elf[seg['offset']:seg['offset'] + seg['filesz']]
    base = seg['vaddr']

    sym = {}
    for i, s in enumerate(m['symbols']):
        if s['identity']:
            sym[i] = f"{s['identity'][0]}#{s['identity'][1][0]}"
    slots = {}
    for target, kind, symid, addend in m['relocs']:
        if symid in sym:
            slots[target] = sym[symid]

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    # import stubs: jmp qword ptr [rip + slot]
    stub = {}
    for i in range(len(code) - 6):
        if code[i] == 0xFF and code[i + 1] == 0x25:
            disp = int.from_bytes(code[i + 2:i + 6], 'little', signed=True)
            addr = base + i
            nm = slots.get(addr + 6 + disp)
            if nm:
                stub[addr] = nm

    for a in args.addresses:
        start = int(a, 0)
        print(f'\n=== function eboot.bin+{start:#x} ===')
        off = start - base
        n, depth_end, seen_calls = 0, None, []
        furthest_jump = start
        for ins in md.disasm(code[off:off + args.max_insns * 15], start):
            n += 1
            if n > args.max_insns:
                print('  (instruction budget reached; function may be longer)')
                break
            if ins.mnemonic == 'call':
                for op in ins.operands:
                    if op.type == CS_OP_IMM:
                        t = op.imm
                        label = stub.get(t)
                        seen_calls.append((ins.address, pretty(label, names_by_nid) if label else f'helper {t:#x}'))
                    elif op.type == CS_OP_MEM and op.mem.base == 0x29:
                        t = ins.address + ins.size + op.mem.disp
                        label = slots.get(t)
                        seen_calls.append((ins.address, pretty(label, names_by_nid) if label else f'indirect [{t:#x}]'))
                    elif op.type == CS_OP_MEM:
                        seen_calls.append((ins.address, 'indirect through a register or table (virtual dispatch)'))
            elif ins.mnemonic.startswith('j'):
                for op in ins.operands:
                    if op.type == CS_OP_IMM and op.imm > furthest_jump:
                        furthest_jump = op.imm
            elif ins.mnemonic == 'ret':
                if ins.address >= furthest_jump:
                    depth_end = ins.address
                    break
        imports = [c for c in seen_calls if '(' in c[1]]
        print(f'  instructions scanned: {n}; ends at {depth_end:#x}' if depth_end else f'  instructions scanned: {n}; end not found')
        print(f'  calls: {len(seen_calls)} total, {len(imports)} into console imports')
        for addr, label in seen_calls:
            mark = ' *' if '(' in label else '  '
            print(f'   {addr:#x}{mark} {label}')


if __name__ == '__main__':
    main()
