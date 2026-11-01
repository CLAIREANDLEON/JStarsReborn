#!/usr/bin/env python3
"""Build the allowlisted foundation payload manifest; never include game data."""
import argparse
import hashlib
import os
from pathlib import Path
import stat
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    args = parser.parse_args()
    root = args.package.resolve(strict=True)
    files = ['JStarsReborn.exe', 'config/settings.ini', 'README.md', 'THIRD_PARTY_NOTICES.md']
    files += [name for name in ['CHANGELOG.md', 'milestones.json'] if (root / name).is_file()]
    for folder, extension in [('docs', '.md'), ('licenses', '.txt')]:
        if (root / folder).is_dir():
            files += [f.relative_to(root).as_posix() for f in (root / folder).iterdir() if f.suffix.lower() == extension and f.is_file()]
    files = sorted(set(files))
    if len(files) > 256:
        raise ValueError('Payload exceeds 256-file budget')
    total = 0
    lines = []
    for relative in files:
        file = root / relative
        for ancestor in [file, *file.parents]:
            if ancestor == root.parent:
                break
            info = ancestor.lstat()
            if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
                raise ValueError('Payload must not contain links/reparse points: ' + relative)
        before = file.stat()
        if not stat.S_ISREG(before.st_mode):
            raise ValueError('Payload entry is not a regular file')
        digest = hashlib.sha256()
        count = 0
        with file.open('rb') as stream:
            while block := stream.read(1024 * 1024):
                count += len(block)
                total += len(block)
                if total > 100 * 1024 * 1024:
                    raise ValueError('Payload exceeds 100 MiB budget')
                digest.update(block)
        after = file.stat()
        if count != before.st_size or (before.st_size, before.st_mtime_ns, before.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino):
            raise ValueError('Payload changed while hashing: ' + relative)
        lines.append(f'{digest.hexdigest()}\t{count}\t{relative}')
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', newline='\n', dir=root, prefix='.payload-', delete=False) as f:
            temporary = Path(f.name)
            f.write('\n'.join(lines) + '\n')
            f.flush()
            os.fsync(f.fileno())
        os.replace(temporary, root / 'setup-payload.txt')
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    print(f'Prepared {len(files)} runtime files, {total} bytes. Game assets are not included.')


if __name__ == '__main__':
    main()
