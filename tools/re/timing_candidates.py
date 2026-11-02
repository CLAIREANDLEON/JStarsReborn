#!/usr/bin/env python3
"""Read-only timing reconnaissance of a user's eboot; emit addresses, not game bytes.

Linear decoding is a candidate finder, not proof of reachable code or function boundaries.
Includes tail jumps to import stubs, which a direct-call-only import map misses.
Output deliberately excludes input paths, usernames, hostnames and disassembly text.
"""
import argparse
import collections
import json
import pathlib
import re
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'prepare'))
from link_modules import module
from map_imports import nid_names
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

VALUES = {'seconds_30hz': 1 / 30, 'seconds_60hz': 1 / 60,
          'seconds_120hz': 1 / 120, 'rate_30hz': 30.0, 'rate_60hz': 60.0,
          'milliseconds_30hz': 1000 / 30, 'milliseconds_60hz': 1000 / 60}
TIMING = re.compile(r'(?:^sceKernel.*(?:Time|Tsc|sleep)|^sceVideoOut.*(?:Flip|Vblank)|'
                    r'^sceGnm.*(?:Flip|TimeStamp)|^(?:clock_gettime|gettimeofday|nanosleep|usleep)$)')
RIP = re.compile(r'\[rip(?: ([+-]) (0x[0-9a-f]+|[0-9]+))?\]')


def rip_target(address, size, operands):
    match = RIP.search(operands)
    if not match:
        return None
    displacement = int(match[2], 0) if match[2] else 0
    return address + size + (-displacement if match[1] == '-' else displacement)


def scan(m, names, targets=()):
    elf = m['elf']
    segments = [p for p in m['ph'] if p['type'] == 1]
    constants = {}
    for seg in segments:
        data = elf[seg['offset']:seg['offset'] + seg['filesz']]
        for label, value in VALUES.items():
            needle = struct.pack('<f', value)
            pos = data.find(needle)
            while pos >= 0:
                address = seg['vaddr'] + pos
                if address % 4 == 0:
                    constants[address] = label
                pos = data.find(needle, pos + 1)
    slots = {}
    for address, kind, symbol, addend in m['relocs']:
        ident = m['symbols'][symbol]['identity']
        if ident:
            name = names.get(ident[0], ident[0])
            if TIMING.search(name):
                slots[address] = name
    stubs = {}
    # Import trampolines are verified against relocation slots, independently of linear decode.
    for seg in segments:
        if not seg['flags'] & 1:
            continue
        data = elf[seg['offset']:seg['offset'] + seg['filesz']]
        for pos in range(len(data) - 5):
            if data[pos:pos + 2] == b'\xff\x25':
                address = seg['vaddr'] + pos
                target = address + 6 + struct.unpack_from('<i', data, pos + 2)[0]
                if target in slots:
                    stubs[address] = slots[target]
    references = collections.defaultdict(list)
    edges = collections.defaultdict(list)
    indirect = []
    counters = []
    target_refs = collections.defaultdict(list)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.skipdata = True
    decoded = 0
    for seg in segments:
        if not seg['flags'] & 1:
            continue
        data = elf[seg['offset']:seg['offset'] + seg['filesz']]
        for address, size, mnemonic, operands in md.disasm_lite(data, seg['vaddr']):
            if mnemonic == '.byte':
                continue
            decoded += 1
            if mnemonic in ('rdtsc', 'rdtscp'):
                counters.append(hex(address))
            target = rip_target(address, size, operands)
            if target in targets:
                target_refs[target].append({'site': hex(address), 'operation': mnemonic})
            if target in constants and ('ss' in mnemonic or 'ps' in mnemonic):
                references[target].append({'site': hex(address), 'operation': mnemonic})
            if target in slots:
                indirect.append({'site': hex(address), 'operation': mnemonic,
                                 'import': slots[target], 'slot': hex(target)})
            if mnemonic in ('call', 'jmp') and re.fullmatch(r'0x[0-9a-f]+', operands):
                edges[int(operands, 16)].append({'site': hex(address), 'operation': mnemonic})
    imports = []
    for stub, name in sorted(stubs.items()):
        uses = edges.get(stub, [])
        wrappers = [{'address': use['site'], 'callers': edges.get(int(use['site'], 16), [])}
                    for use in uses if use['operation'] == 'jmp']
        imports.append({'import': name, 'stub': hex(stub), 'branches': uses,
                        'tail_wrapper_candidates': wrappers})
    return {'schema': 1, 'eboot_sha256': m['sha256'],
            'method': 'linear decode candidates; validate control flow and live hits before patching',
            'instructions_decoded': decoded, 'timing_imports': imports,
            'timing_slot_references': indirect, 'hardware_counter_sites': counters,
            'requested_references': [{'address': hex(a), 'memory': target_refs[a],
                                      'direct_branches': edges.get(a, [])}
                                     for a in targets],
            'float_references': [{'address': hex(a), 'value': constants[a], 'sites': refs}
                                 for a, refs in sorted(references.items())]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=pathlib.Path)
    parser.add_argument('--out', type=pathlib.Path, required=True)
    parser.add_argument('--address', type=lambda s: int(s, 0), action='append', default=[],
                        help='Additional RVA to find RIP-relative and direct branch references to')
    args = parser.parse_args()
    m = module(args.game / 'eboot.bin')
    if any(m['ph'][i]['type'] == 1 for i in m['missing']):
        parser.error('Input has unavailable SELF segments; use a complete decrypted dump')
    report = scan(m, nid_names(pathlib.Path(__file__).resolve().parents[2]), args.address)
    args.out.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"Timing reconnaissance: {len(report['timing_imports'])} imports, "
          f"{len(report['float_references'])} referenced float candidates; no game bytes emitted")


if __name__ == '__main__':
    main()
