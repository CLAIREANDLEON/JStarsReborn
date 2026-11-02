"""Build the Windows runtime with llvm-mingw, without make or a Unix shell.

The default host-service build does not render. --gpu-library additionally links
a renderer built against the same C++ runtime as llvm-mingw (libc++).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from datetime import datetime, timezone


def sha256(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--llvm-bin', type=Path, required=True,
                        help='llvm-mingw bin directory (x86_64 Windows target)')
    parser.add_argument('--out-dir', type=Path, required=True,
                        help='New or empty output directory; never overwrites a build')
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--gpu-library', type=Path,
                        help='Optional compatible libc++ renderer .a; GCC/libstdc++ archives are incompatible')
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('This entry point is for native Windows builds.')
    source, out, llvm = args.source.resolve(), args.out_dir.resolve(), args.llvm_bin.resolve()
    runtime = source / 'runtime'
    if source == out or source in out.parents:
        parser.error('--out-dir must be outside the source tree')
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        parser.error('--out-dir must be new or empty')
    clang, cxx = llvm / 'clang.exe', llvm / 'clang++.exe'
    if not clang.is_file() or not cxx.is_file():
        parser.error('--llvm-bin must contain clang.exe and clang++.exe')
    makefile = runtime / 'Makefile'
    # Read the authoritative source list without executing Makefile/shell code.
    text = makefile.read_text(encoding='utf-8').replace('\\\n', ' ')
    match = re.search(r'^SRC\s*=\s*(.+)$', text, re.M)
    if not match:
        parser.error('Cannot find the runtime Makefile SRC list')
    names = match[1].split('#', 1)[0].split()
    if not names or any(not re.fullmatch(r'[A-Za-z0-9_]+\.c', name) for name in names):
        parser.error('Unsupported Makefile SRC list; review the build script before proceeding')
    sources = [runtime / name for name in names]
    if any(not path.is_file() for path in sources):
        parser.error('A runtime source file is missing')
    gpu = args.gpu_library.resolve() if args.gpu_library else None
    if gpu and not gpu.is_file():
        parser.error('--gpu-library does not exist')
    out.mkdir(parents=True, exist_ok=True)
    manifest = {
        'started_utc': datetime.now(timezone.utc).isoformat(),
        'source': str(source), 'mode': 'gpu' if gpu else 'host-services',
        'build_succeeded': False, 'runtime_tested': False,
        'compiler': {'path': str(clang), 'sha256': sha256(clang)},
        'inputs': {str(path.relative_to(source)): sha256(path)
                   for path in sources + sorted(runtime.glob('*.h')) + [makefile]},
        'steps': [],
    }
    if gpu:
        manifest['renderer'] = {'path': str(gpu), 'sha256': sha256(gpu)}
        for header in (source / 'gpu').glob('*.h'):
            manifest['inputs'][str(header.relative_to(source))] = sha256(header)

    def run(label: str, argv: list[str]) -> int:
        log = out / f'{label}.log'
        print(f'{label}: {subprocess.list2cmdline(argv)}', flush=True)
        with log.open('wb') as stream:
            proc = subprocess.run(argv, cwd=out, stdout=stream, stderr=subprocess.STDOUT)
        manifest['steps'].append({'name': label, 'argv': argv,
                                  'exit': proc.returncode, 'log': log.name})
        return proc.returncode

    try:
        if run('compiler-version', [str(clang), '--version']):
            return 1
        flags = ['--target=x86_64-w64-windows-gnu', '-std=gnu2x', '-O2', '-g', '-Wall', '-Wextra']
        if gpu:
            flags += ['-DJSTARS_GPU', '-I' + str(source / 'gpu')]
        if run('compile', [str(clang), *flags, '-c', *map(str, sources)]):
            return 1
        exe = out / ('jstars-boot-gpu-native.exe' if gpu else 'jstars-boot-native.exe')
        objects = [str(out / (Path(name).stem + '.o')) for name in names]
        libraries = ['-lws2_32', '-lole32', '-luser32', '-ldbghelp', '-lshell32', '-lpthread', '-lm']
        link = [str(cxx if gpu else clang), '--target=x86_64-w64-windows-gnu', '-static',
                '-o', str(exe), *objects]
        if gpu:
            link += ['-Wl,--allow-multiple-definition', str(gpu), '-lmswsock', '-lgdi32',
                     '-lbcrypt', '-lshlwapi', '-ladvapi32', '-lwinmm', '-lntdll']
        if run('link', link + libraries):
            print('Link failed; see link.log. The exported GCC/libstdc++ renderer is not '
                  'compatible with llvm-mingw libc++.', file=sys.stderr)
            return 1
        manifest['build_succeeded'] = True
        manifest['artifact'] = {'path': str(exe), 'sha256': sha256(exe)}
        print(f'Built {exe}\nSHA-256 {manifest["artifact"]["sha256"]}')
        return 0
    except OSError as exc:
        manifest['error'] = str(exc)
        print(str(exc), file=sys.stderr)
        return 1
    finally:
        manifest['finished_utc'] = datetime.now(timezone.utc).isoformat()
        (out / 'build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    raise SystemExit(main())
