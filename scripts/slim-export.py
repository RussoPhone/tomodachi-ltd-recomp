#!/usr/bin/env python3
"""Removes the JIT hand-off guards from the generated C before it is compiled.

The recompiler emits, at the top of every block, a copy of the block's instruction words (`static const uint32_t _expected[] = {...};`)
and a guard that re-checks them (`RECOMP_GG_GUARD(_expected, n);`), so the code could fall back to a JIT if the guest code changed.
nxrt has no JIT and the game's code never changes, so both are dead weight: removing them makes the generated C ~9% smaller and
the objects ~35% smaller, and compiles faster.

  python3 scripts/slim-export.py <exefs folder of the export>     (idempotent; writes <folder>/.slimmed when done)

It also puts calls to nxrt's host functions at the entry of a few game functions (HOOKS below): today the hand-pointer update, so the
mouse can steer the game's hand. The calls are plain C added to the generated source on your computer.
"""
import multiprocessing
import os
from pathlib import Path
import re
import sys

EXPECTED = re.compile(rb'^[ \t]*static const uint32_t _expected\[\][^\n]*\n', re.M)
GUARD = re.compile(rb'^[ \t]*RECOMP_GG_GUARD\([^\n]*\n', re.M)
MARKER = '.slimmed'
# (module, function offset in the module, host function). The host function takes the GuestContext and runs when the guest function
# is entered at its first instruction (never when a call returns into the middle of it).
HOOKS = [('main', 0x22dce0, 'nxrt_hook_pointer')]
HOOK_VERSION = 1


def slim_text(data: bytes):
    """Returns (new bytes, expected lines removed, guard lines removed). A file with a guard but no matching array is left alone."""
    expected = len(EXPECTED.findall(data))
    guards = len(GUARD.findall(data))
    if expected != guards:
        raise ValueError(f'{expected} _expected arrays but {guards} guards')
    return GUARD.sub(b'', EXPECTED.sub(b'', data)), expected, guards


def apply_hooks(data: bytes) -> bytes:
    for module, offset, function in HOOKS:
        header = f'void blk_{module}_{offset:016x}(GuestContext* c){{\n'.encode()
        at = data.find(header)
        call = f'    {function}(c);\n'.encode()
        if at < 0 or call in data[at:at + 20000]:
            continue
        end = data.find(b'    } } }\n', at)
        if end < 0:
            raise ValueError(f'dispatcher of blk_{module}_{offset:016x} not found')
        end += len(b'    } } }\n')
        data = data[:at] + f'void {function}(GuestContext* c);\n'.encode() + data[at:end] + call + data[end:]
    return data


def slim_file(path):
    path = Path(path)
    data = path.read_bytes()
    hooked = apply_hooks(data)
    if b'RECOMP_GG_GUARD(' not in data and b'_expected[]' not in data:
        if hooked != data:
            path.with_suffix('.c.tmp').write_bytes(hooked)
            path.with_suffix('.c.tmp').replace(path)
        return str(path), 0, len(data), len(hooked)
    new, removed, _ = slim_text(hooked)
    tmp = path.with_suffix('.c.tmp')
    tmp.write_bytes(new)
    tmp.replace(path)
    return str(path), removed, len(data), len(new)


def slim_folder(folder, jobs=None):
    folder = Path(folder)
    marker = folder / MARKER
    if marker.exists() and f'hooks={HOOK_VERSION}' in marker.read_text():
        return None
    files = sorted(p for p in folder.rglob('*.c') if p.is_file())
    if not files:
        raise SystemExit(f'No .c files under {folder}')
    with multiprocessing.get_context('fork').Pool(jobs or os.cpu_count() or 1) as pool:
        results = pool.map(slim_file, files, chunksize=8)
    before, after = sum(r[2] for r in results), sum(r[3] for r in results)
    marker.write_text(f'{len(files)} files, {before} -> {after} bytes\nhooks={HOOK_VERSION}\n')
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
