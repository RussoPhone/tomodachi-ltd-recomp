#!/usr/bin/env python3
"""Removes the JIT hand-off guards from the generated C before it is compiled.

The recompiler emits, at the top of every block, a copy of the block's instruction words (`static const uint32_t _expected[] = {...};`)
and a guard that re-checks them (`RECOMP_GG_GUARD(_expected, n);`), so the code could fall back to a JIT if the guest code changed.
nxrt has no JIT and the game's code never changes, so both are dead weight: removing them makes the generated C ~9% smaller and
the objects ~35% smaller, and compiles faster.

  python3 scripts/slim-export.py <exefs folder of the export>     (idempotent; writes <folder>/.slimmed when done)
"""
import multiprocessing
import os
from pathlib import Path
import re
import sys

EXPECTED = re.compile(rb'^[ \t]*static const uint32_t _expected\[\][^\n]*\n', re.M)
GUARD = re.compile(rb'^[ \t]*RECOMP_GG_GUARD\([^\n]*\n', re.M)
MARKER = '.slimmed'


def slim_text(data: bytes):
    """Returns (new bytes, expected lines removed, guard lines removed). A file with a guard but no matching array is left alone."""
    expected = len(EXPECTED.findall(data))
    guards = len(GUARD.findall(data))
    if expected != guards:
        raise ValueError(f'{expected} _expected arrays but {guards} guards')
    return GUARD.sub(b'', EXPECTED.sub(b'', data)), expected, guards


def slim_file(path):
    path = Path(path)
    data = path.read_bytes()
    if b'RECOMP_GG_GUARD(' not in data and b'_expected[]' not in data:
        return str(path), 0, len(data), len(data)
    new, removed, _ = slim_text(data)
    tmp = path.with_suffix('.c.tmp')
    tmp.write_bytes(new)
    tmp.replace(path)
    return str(path), removed, len(data), len(new)


def slim_folder(folder, jobs=None):
    folder = Path(folder)
    if (folder / MARKER).exists():
        return None
    files = sorted(p for p in folder.rglob('*.c') if p.is_file())
    if not files:
        raise SystemExit(f'No .c files under {folder}')
    with multiprocessing.get_context('fork').Pool(jobs or os.cpu_count() or 1) as pool:
        results = pool.map(slim_file, files, chunksize=8)
    before, after = sum(r[2] for r in results), sum(r[3] for r in results)
    (folder / MARKER).write_text(f'{len(files)} files, {before} -> {after} bytes\n')
    return len(files), before, after


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    result = slim_folder(sys.argv[1])
    if result is None:
        print('already slimmed')
    else:
        files, before, after = result
        print(f'slimmed {files} files: {before / 1e6:.0f} MB -> {after / 1e6:.0f} MB')


if __name__ == '__main__':
    main()
