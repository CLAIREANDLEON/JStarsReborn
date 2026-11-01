#!/usr/bin/env python3
"""Write import-names.tsv (NID#library -> symbol name) for diagnostics only.

Source of names: the shadPS4 project's public NID table (GPL-2.0-or-later, aerolib.inl). A
name is a label for a hash; it never changes what the runtime does. Imports whose NID is not
in the table stay unnamed in logs. The table path is a parameter; nothing here touches the game.
"""
import json
import re
import sys
from pathlib import Path

def main(link_json, aerolib, out):
    names = {}
    for m in re.finditer(r'STUB\("([^"]+)",\s*([^)\s]+)\)', Path(aerolib).read_text(errors='replace')):
        names[m.group(1)] = m.group(2)
    report = json.loads(Path(link_json).read_text())
    boot = Path(link_json).with_name('boot-linked.bin')
    rows, known = [], 0
    for imp in report['all_imports']:
        nid = imp.split('#')[0]
        name = names.get(nid, '')
        known += bool(name)
        rows.append(f'{imp}\t{name}')
    Path(out).write_text('\n'.join(rows) + '\n')
    print(f'{known}/{len(rows)} imports have a name in the NID table -> {out}')

if __name__ == '__main__':
    main(*sys.argv[1:4])
