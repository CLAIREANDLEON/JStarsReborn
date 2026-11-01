#!/usr/bin/env python3
"""Write checkpoint-manifest.json for a package folder. Run AFTER make_setup_payload.py.

Excluded: build outputs, runtime data (logs, reports, saves, crashes), VCS and caches.
added/changed/removed are computed against the previous manifest in the same folder.
"""
import argparse, hashlib, json, os
from pathlib import Path

EXCLUDE_DIRS = {'build', '.git', '__pycache__', 'logs', 'reports', 'saves', 'crashes'}
EXCLUDE_FILES = {'checkpoint-manifest.json', 'jstars-boot'}

def digest(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--package', type=Path, required=True)
    ap.add_argument('--checkpoint', required=True)
    ap.add_argument('--scope', required=True)
    ap.add_argument('--date', required=True)
    ap.add_argument('--binaries-note', default='')
    ap.add_argument('--base-manifest', type=Path, help='previous checkpoint manifest to diff against (default: the one in the package)')
    a = ap.parse_args()
    root = a.package.resolve(strict=True)
    old = {}
    prev = root / 'checkpoint-manifest.json'
    base = None
    source = a.base_manifest or prev
    if source.is_file():
        d = json.loads(source.read_text())
        base = d.get('checkpoint')
        old = {f['path']: f['sha256'] for f in d['files']}
    files = []
    for dirpath, dirs, names in os.walk(root):
        dirs[:] = sorted(x for x in dirs if x not in EXCLUDE_DIRS)
        for n in sorted(names):
            p = Path(dirpath) / n
            rel = p.relative_to(root).as_posix()
            if n in EXCLUDE_FILES or p.is_symlink():
                continue
            files.append({'path': rel, 'size_bytes': p.stat().st_size, 'sha256': digest(p)})
    files.sort(key=lambda f: f['path'])
    new = {f['path']: f['sha256'] for f in files}
    out = {
        'schema_version': 1, 'checkpoint': a.checkpoint, 'base_checkpoint': base, 'date': a.date,
        'scope': a.scope, 'binaries_note': a.binaries_note,
        'runtime_data_excluded': ['logs', 'reports', 'saves', 'crashes', 'build outputs'],
        'added_files': sorted(set(new) - set(old)),
        'changed_files': sorted(k for k in new if k in old and new[k] != old[k]),
        'removed_files': sorted(set(old) - set(new)),
        'files': files,
    }
    prev.write_text(json.dumps(out, indent=2) + '\n')
    print(f"{len(files)} files; +{len(out['added_files'])} ~{len(out['changed_files'])} -{len(out['removed_files'])}")

if __name__ == '__main__':
    main()
