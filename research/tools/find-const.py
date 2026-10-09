#!/usr/bin/env python3
"""Find the functions that build a given 32/64-bit constant (MOVZ/MOVN + MOVK sequences, or a literal word).

Hashes, magic numbers and enum values compiled into code are a strong link between a producer and its
consumers (e.g. a request posted under a type hash and the code that handles that hash).

  python3 scripts/find-const.py 0x6c74030d [--module main]
"""
import argparse
import bisect
import importlib.util
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import lab  # noqa: E402

spec = importlib.util.spec_from_file_location('inventory', ROOT / 'scripts/inventory-modules.py')
inventory = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inventory)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('value')
    p.add_argument('--module', default='main')
    a = p.parse_args()
    value = int(a.value, 0)
    nso = inventory.Nso(ROOT / f'local/runtime/user/dump/{lab.title_id()}/exefs/{a.module}')
    image = nso.flat()
    private = ROOT / f'local/analysis/{lab.target_name()}'
    funcs = sorted((int(json.loads(l)['offset'], 16), json.loads(l)['size'])
                   for l in (private / 'functions' / f'{a.module}.jsonl').read_text().splitlines() if l.strip())
    starts = [s for s, _ in funcs]
    names = {}
    for f in ('data-readers.jsonl', 'classes.jsonl', 'manual.jsonl'):
        path = private / 'names' / f
        if path.exists():
            for line in path.read_text().splitlines():
                r = json.loads(line)
                names[int(r['offset'], 16)] = r['name']
    words = struct.unpack_from(f'<{(nso.text.end - nso.text.vaddr) // 4}I', image, nso.text.vaddr)
    regs = {}
    hits = {}
    for i, w in enumerate(words):
        pc = nso.text.vaddr + i * 4
        if (w & 0x7F800000) == 0x52800000:      # MOVZ
            hw = (w >> 21) & 3
            regs[w & 31] = ((w >> 5) & 0xFFFF) << (16 * hw)
        elif (w & 0x7F800000) == 0x12800000:    # MOVN
            hw = (w >> 21) & 3
            regs[w & 31] = ~(((w >> 5) & 0xFFFF) << (16 * hw)) & (0xFFFFFFFF if not w >> 31 else (1 << 64) - 1)
        elif (w & 0x7F800000) == 0x72800000 and (w & 31) in regs:   # MOVK
            hw = (w >> 21) & 3
            regs[w & 31] = (regs[w & 31] & ~(0xFFFF << (16 * hw))) | (((w >> 5) & 0xFFFF) << (16 * hw))
        else:
            continue
        if regs.get(w & 31) in (value, value & 0xFFFFFFFF):
            j = bisect.bisect_right(starts, pc) - 1
            f = starts[j] if j >= 0 and pc < starts[j] + funcs[j][1] else None
            hits.setdefault(f, []).append(pc)
    for f, pcs in sorted(hits.items(), key=lambda x: x[0] or 0):
        print(f'{f:#x} {names.get(f, "") if f is not None else "(outside functions)"}  at ' +
              ', '.join(f'{pc:#x}' for pc in pcs[:4]))
    print(f'{len(hits)} function(s)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
