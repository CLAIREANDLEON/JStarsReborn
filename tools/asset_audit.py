#!/usr/bin/env python3
"""Read-only inventory of supplied local files, NOT a PS4 asset parser/importer."""
from __future__ import annotations
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import stat
import sys
import tempfile

BLOCK_SIZE = 1024 * 1024
REPARSE_POINT = 0x400


def is_link(info: os.stat_result) -> bool:
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, 'st_file_attributes', 0) & REPARSE_POINT)


def below(candidate: Path, parent: Path) -> bool:
    try:
        candidate.relative_to(parent)
        return True
    except ValueError:
        return False


def checked_path(value: Path) -> Path:
    """Reject existing symbolic links/reparse points throughout the supplied path."""
    value = Path(os.path.abspath(value))
    for item in reversed((value, *value.parents)):
        try:
            info = item.lstat()
        except FileNotFoundError:
            continue
        if is_link(info):
            raise ValueError('Symbolic links and reparse points are not accepted: ' + str(item))
    return value


def signature(head: bytes) -> str:
    """A signature hint alone is never proof of valid or supported asset content."""
    if head.startswith(b'\x89PNG\r\n\x1a\n'):
        return 'png-signature'
    if head.startswith(b'DDS '):
        return 'dds-signature'
    if head.startswith(b'\xabKTX 20\xbb\r\n\x1a\n'):
        return 'ktx2-signature'
    if head[:4] == b'RIFF' and head[8:12] == b'WAVE':
        return 'riff-wave-signature'
    if head.startswith(b'OggS'):
        return 'ogg-signature'
    if head.startswith(b'fLaC'):
        return 'flac-signature'
    if head.startswith(b'\x7fELF'):
        return 'elf-signature-unparsed'
    if head.startswith(b'MZ'):
        return 'mz-signature-unparsed'
    return 'unknown'


def identity(info: os.stat_result) -> tuple:
    stable = (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns)
    # Windows ctime is deprecated and differs between lstat/fstat on this host.
    # It is not a reliable cross-API change counter; POSIX ctime remains useful.
    return stable if os.name == 'nt' else stable + (info.st_ctime_ns,)


def verify_open_path(fd: int, expected: Path) -> None:
    """Check the opened object before reading, catching ancestor-link substitutions."""
    if os.name == 'nt':
        import msvcrt
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        get_name = kernel.GetFinalPathNameByHandleW
        get_name.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32]
        get_name.restype = ctypes.c_uint32
        buffer = ctypes.create_unicode_buffer(32768)
        size = get_name(msvcrt.get_osfhandle(fd), buffer, len(buffer), 0)
        if size == 0 or size >= len(buffer):
            raise OSError('Cannot verify the opened file path')
        opened = buffer.value
        if opened.startswith('\\\\?\\UNC\\'):
            opened = '\\\\' + opened[8:]
        elif opened.startswith('\\\\?\\'):
            opened = opened[4:]
        if os.path.normcase(opened) != os.path.normcase(str(expected)):
            raise OSError('Opened path differs from requested path')
    elif Path('/proc/self/fd').is_dir():
        if Path(os.readlink('/proc/self/fd/' + str(fd))) != expected:
            raise OSError('Opened path differs from requested path')
    else:
        raise OSError('Secure opened-path verification is unavailable on this platform')


def hash_file(item: Path, before: os.stat_result, max_file_bytes: int, budget: int, on_read=None) -> dict:
    if before.st_size > min(max_file_bytes, budget):
        raise ValueError('File exceeds configured per-file or remaining total byte limit')
    flags = os.O_RDONLY | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NOFOLLOW', 0)
    fd = os.open(item, flags)
    try:
        opened = os.fstat(fd)
        if not stat.S_ISREG(opened.st_mode) or is_link(opened) or identity(opened) != identity(before):
            raise OSError('File changed between inspection and opening: ' + repr(identity(before)) + ' -> ' + repr(identity(opened)))
        verify_open_path(fd, item)
        digest = hashlib.sha256()
        total = 0
        head = b''
        while total < before.st_size:
            chunk = os.read(fd, min(BLOCK_SIZE, before.st_size - total))
            if not chunk:
                break
            total += len(chunk)
            if on_read is not None:
                on_read(len(chunk))
            if len(head) < 64:
                head += chunk[:64 - len(head)]
            digest.update(chunk)
        after = os.fstat(fd)
        current = item.lstat()
        if total != before.st_size or identity(before) != identity(after) or identity(before) != identity(current) or is_link(current):
            raise OSError('File changed while being hashed: ' + repr(identity(before)) + ' -> ' + repr(identity(after)) + ' -> ' + repr(identity(current)))
        verify_open_path(fd, item)
        return {'size_bytes': total, 'sha256': digest.hexdigest(), 'first_bytes_hex': head.hex(),
                'signature_hint': signature(head), 'validated_asset': False}
    finally:
        os.close(fd)


def audit(root: Path, *, max_entries: int = 100000, max_file_bytes: int = 4 * 1024**3,
          max_total_bytes: int = 64 * 1024**3, max_depth: int = 32) -> dict:
    for name, value in [('max_entries', max_entries), ('max_file_bytes', max_file_bytes),
                        ('max_total_bytes', max_total_bytes), ('max_depth', max_depth)]:
        if type(value) is not int or value <= 0:
            raise ValueError(name + ' must be a positive integer')
    root = checked_path(root)
    if not root.is_dir():
        raise ValueError('Input must be an existing directory')
    report = {'schema': 'reborn.local-inventory.v1', 'input_root': str(root),
              'complete': True, 'bytes_hashed': 0, 'bytes_read': 0, 'entries_seen': 0,
              'limitations': ['Signature hints are not asset validation or format support.',
                              'No proprietary executable is executed, decrypted, or translated.',
                              'Concurrent malicious rewrites cannot be proven absent; use a static copy.'],
              'limits': {'max_entries': max_entries, 'max_file_bytes': max_file_bytes,
                         'max_total_bytes': max_total_bytes, 'max_depth': max_depth},
              'files': [], 'diagnostics': []}
    def error(item: Path, message: str) -> None:
        report['complete'] = False
        report['diagnostics'].append({'path': item.relative_to(root).as_posix(), 'error': message})
    def count_read(count: int) -> None:
        report['bytes_read'] += count
    pending = [(root, 0)]
    stopped = False
    while pending and not stopped:
        folder, depth = pending.pop()
        try:
            checked_path(folder)
            entries = []
            with os.scandir(folder) as iterator:
                for entry in iterator:
                    if report['entries_seen'] >= max_entries:
                        error(folder, 'Maximum directory-entry limit reached; inventory incomplete')
                        stopped = True
                        break
                    report['entries_seen'] += 1
                    entries.append(entry.name)
            for name in sorted(entries):
                item = folder / name
                try:
                    info = item.lstat()
                    if is_link(info):
                        error(item, 'Skipped symbolic link or reparse point')
                    elif stat.S_ISDIR(info.st_mode):
                        if depth >= max_depth:
                            error(item, 'Maximum traversal depth reached')
                        else:
                            pending.append((item, depth + 1))
                    elif stat.S_ISREG(info.st_mode):
                        checked_path(item)
                        metadata = hash_file(item, info, max_file_bytes, max_total_bytes - report['bytes_read'], count_read)
                        metadata['path'] = item.relative_to(root).as_posix()
                        metadata['extension_hint'] = item.suffix.lower()
                        report['bytes_hashed'] += metadata['size_bytes']
                        report['files'].append(metadata)
                    else:
                        error(item, 'Skipped non-regular filesystem object')
                except (OSError, ValueError) as exc:
                    error(item, str(exc))
        except (OSError, ValueError) as exc:
            error(folder, str(exc))
    report['files'].sort(key=lambda entry: entry['path'])
    report['diagnostics'].sort(key=lambda entry: (entry['path'], entry['error']))
    return report


def write_report(root: Path, destination: Path, report: dict) -> None:
    root = checked_path(root)
    destination = checked_path(destination)
    if below(destination, root):
        raise ValueError('Report output must be outside the input directory')
    if not destination.parent.is_dir():
        raise ValueError('Report output parent directory must already exist')
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=destination.parent,
                                         prefix='.reborn-inventory-', suffix='.tmp', delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(report, stream, indent=2, sort_keys=True)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        # Recheck before committing; do not follow a replaced output ancestor.
        checked_path(destination)
        os.replace(temporary, destination)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def positive(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError('must be a positive integer')
    return parsed


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path, help='Legitimately supplied local data directory')
    parser.add_argument('--output', required=True, type=Path, help='JSON report path outside input')
    parser.add_argument('--max-entries', type=positive, default=100000)
    parser.add_argument('--max-file-bytes', type=positive, default=4 * 1024**3)
    parser.add_argument('--max-total-bytes', type=positive, default=64 * 1024**3)
    parser.add_argument('--max-depth', type=positive, default=32)
    args = parser.parse_args(argv)
    try:
        root = checked_path(args.input)
        output = checked_path(args.output)
        if below(output, root):
            raise ValueError('Report output must be outside the input directory')
        report = audit(root, max_entries=args.max_entries, max_file_bytes=args.max_file_bytes,
                       max_total_bytes=args.max_total_bytes, max_depth=args.max_depth)
        write_report(root, output, report)
        print('Inventory written: ' + str(output))
        print(str(len(report['files'])) + ' files hashed; ' + str(len(report['diagnostics'])) + ' diagnostics')
        return 0 if report['complete'] else 1
    except (OSError, ValueError) as exc:
        print('Inventory failed: ' + str(exc), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
