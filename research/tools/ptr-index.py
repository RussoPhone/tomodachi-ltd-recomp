#!/usr/bin/env python3
"""Index function pointers stored in data (vtables, callback tables) and the code that installs them.

Virtual functions have no direct callers, so a call graph built from BL/B stops at them. Every pointer
stored in the module's data is a dynamic relocation (the NSO is position independent), so:
  1. R_AARCH64_RELATIVE entries of DT_RELA and every DT_RELR entry give (slot address -> target);
  2. targets that are function starts (.eh_frame FunctionRecords) are code pointers;
  3. runs of consecutive code-pointer slots are tables, mostly C++ vtables (address point = first slot);
  4. code that materialises a table address with ADRP+ADD is what installs it (constructors, destructors).
So for any virtual function: table + slot index, and the functions that construct objects of that class.

  python3 scripts/ptr-index.py build [--module main]
  python3 scripts/ptr-index.py func <0xoffset>     tables holding the function, and their installers
  python3 scripts/ptr-index.py table <0xaddr>      slots of a table and its installers

Output: local/analysis/<target>/index/<module>.ptrs.json (private).
"""
import argparse
import bisect
import collections
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

DT_RELA, DT_RELASZ, DT_RELRSZ, DT_RELR = 7, 8, 35, 36
R_AARCH64_RELATIVE = 1027


def sxt(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def dynamic_tags(image, dynamic):
    tags = {}
    off = dynamic
    while off + 16 <= len(image):
        tag, value = struct.unpack_from('<qQ', image, off)
        if tag == 0:
            break
        tags.setdefault(tag, value)
        off += 16
    return tags


def pointers(image, tags):
    """Slot address -> pointed-to module offset, from RELATIVE relocations."""
    out = {}
    if DT_RELA in tags:
        for off in range(tags[DT_RELA], tags[DT_RELA] + tags.get(DT_RELASZ, 0), 24):
            r_offset, r_info, addend = struct.unpack_from('<QQq', image, off)
            if r_info & 0xFFFFFFFF == R_AARCH64_RELATIVE:
                out[r_offset] = addend
    if DT_RELR in tags:  # SHT_RELR: an address, then bitmaps of the following 63 words
        where = 0
        for off in range(tags[DT_RELR], tags[DT_RELR] + tags.get(DT_RELRSZ, 0), 8):
            entry = struct.unpack_from('<Q', image, off)[0]
            if entry & 1 == 0:
                where = entry
                out[where] = struct.unpack_from('<Q', image, where)[0]
                where += 8
            else:
                bits = entry >> 1
                for i in range(63):
                    if bits >> i & 1:
                        slot = where + i * 8
                        out[slot] = struct.unpack_from('<Q', image, slot)[0]
                where += 63 * 8
    return out


def code_refs(image, nso, funcs):
    """(function, data address) for every ADRP+ADD pair in code."""
    starts = [s for s, _ in funcs]
    words = struct.unpack_from(f'<{(nso.text.end - nso.text.vaddr) // 4}I', image, nso.text.vaddr)
    refs = collections.defaultdict(set)
    adrp = {}
    fi, current = 0, None
    for i, w in enumerate(words):
        pc = nso.text.vaddr + i * 4
        j = bisect.bisect_right(starts, pc) - 1
        func = starts[j] if j >= 0 and pc < starts[j] + funcs[j][1] else None
        if func != current:
            adrp.clear()
            current = func
        if func is None:
            continue
        if (w & 0x9F000000) == 0x90000000:
            adrp[w & 31] = (pc & ~0xFFF) + (sxt(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3), 21) << 12)
        elif (w & 0xFF800000) == 0x91000000 and ((w >> 5) & 31) in adrp:
            refs[adrp[(w >> 5) & 31] + ((w >> 10) & 0xFFF) * (4096 if (w >> 22) & 1 else 1)].add(func)
    return refs


def path(module):
    return ROOT / f'local/analysis/{lab.target_name()}/index/{module}.ptrs.json'


def build(module):
    nso = inventory.Nso(ROOT / f'local/runtime/user/dump/{lab.title_id()}/exefs/{module}')
    image = nso.flat()
    _, mod0 = inventory.parse_mod0(image)
    private = ROOT / f'local/analysis/{lab.target_name()}'
    funcs = sorted((int(json.loads(l)['offset'], 16), json.loads(l)['size'])
                   for l in (private / 'functions' / f'{module}.jsonl').read_text().splitlines() if l.strip())
    is_func = {s for s, _ in funcs}
    ptrs = pointers(image, dynamic_tags(image, mod0['dynamic']))
    code_ptrs = {slot: target for slot, target in ptrs.items() if target in is_func}
    # Tables: maximal runs of consecutive code-pointer slots; a run starts where the previous slot is not one.
    tables = {}
    for slot in sorted(code_ptrs):
        if slot - 8 in code_ptrs:
            continue
        run = []
        s = slot
        while s in code_ptrs:
            run.append(code_ptrs[s])
            s += 8
        tables[slot] = run
    refs = code_refs(image, nso, funcs)
    doc = {'module': module, 'pointers': len(ptrs), 'code_pointers': len(code_ptrs),
           'tables': {f'{t:#x}': {'slots': [f'{f:#x}' for f in run],
                                  'installers': sorted(f'{f:#x}' for f in refs.get(t, ()))}
                      for t, run in tables.items()}}
    path(module).write_text(json.dumps(doc))
    multi = sum(1 for r in tables.values() if len(r) >= 2)
    installed = sum(1 for t in tables if refs.get(t))
    print(f'{module}: {len(ptrs)} relocated pointers, {len(code_ptrs)} to functions, '
          f'{len(tables)} tables ({multi} with 2+ slots, {installed} installed by code)')


def load(module):
    return json.loads(path(module).read_text())


def main():
    p = argparse.ArgumentParser()
    p.add_argument('command', choices=['build', 'func', 'table'])
    p.add_argument('arg', nargs='?')
    p.add_argument('--module', default='main')
    a = p.parse_args()
    if a.command == 'build':
        build(a.module)
        return 0
    doc = load(a.module)
    if a.command == 'func':
        key = f'{int(a.arg, 16):#x}'
        for t, rec in doc['tables'].items():
            if key in rec['slots']:
                print(json.dumps({'table': t, 'slot': rec['slots'].index(key), 'size': len(rec['slots']),
                                  'installers': rec['installers'][:20]}))
    else:
        print(json.dumps(doc['tables'].get(f'{int(a.arg, 16):#x}'), indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
