#!/usr/bin/env python3
"""Build a function-level call/reference index of a module for targeted decomp, and query it.

Index (one pass over .text, no disassembler needed):
  - direct calls and tail calls (BL / B imm26) between functions
  - calls into imports: PLT stubs resolved through DT_JMPREL to the imported symbol name
  - references to C strings and data (ADRP + ADD/LDR immediate on the same register)
Every instruction is attributed to its .eh_frame function (FunctionRecord).

  python3 scripts/decomp-index.py build [--module main]
  python3 scripts/decomp-index.py string <text>        functions referencing strings containing <text>
  python3 scripts/decomp-index.py import <text>        functions calling imports whose name contains <text>
  python3 scripts/decomp-index.py func <0xoffset|name> callers, callees, imports and strings of one function
  python3 scripts/decomp-index.py top [n]              most-called functions

Index files live in local/analysis/<target>/index/ (private: they quote the game's strings and names).
"""
import argparse
import bisect
import collections
import importlib.util
import json
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import lab  # noqa: E402

spec = importlib.util.spec_from_file_location('inventory', ROOT / 'scripts/inventory-modules.py')
inventory = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inventory)


def index_dir():
    d = ROOT / f'local/analysis/{lab.target_name()}/index'
    d.mkdir(parents=True, exist_ok=True)
    return d


def sxt(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def plt_imports(image, mod0, text_lo, text_hi):
    """PLT stub address -> imported symbol name, via DT_JMPREL/DT_SYMTAB/DT_STRTAB."""
    tags, off = {}, mod0['dynamic']
    while True:
        tag, val = struct.unpack_from('<qQ', image, off)
        if tag == 0:
            break
        tags.setdefault(tag, val)
        off += 16
    jmprel, size, symtab, strtab = tags.get(23), tags.get(2, 0), tags.get(6), tags.get(5)
    got_to_name = {}
    for i in range(0, size, 24):
        r_offset, r_info, _ = struct.unpack_from('<QQq', image, jmprel + i)
        name_off = struct.unpack_from('<I', image, symtab + (r_info >> 32) * 24)[0]
        end = image.index(b'\0', strtab + name_off)
        got_to_name[r_offset] = image[strtab + name_off:end].decode('utf-8', 'replace')
    stubs = {}
    # stub: ADRP x16, page ; LDR x17, [x16, #off] ; ADD x16, x16, #off ; BR x17
    for pc in range(text_lo, text_hi - 12, 4):
        w0, w1 = struct.unpack_from('<II', image, pc)
        if (w0 & 0x9F00001F) == 0x90000010 and (w1 & 0xFFC003FF) == 0xF9400211:
            page = (pc & ~0xFFF) + (sxt(((w0 >> 5) & 0x7FFFF) << 2 | ((w0 >> 29) & 3), 21) << 12)
            got = page + ((w1 >> 10) & 0xFFF) * 8
            if got in got_to_name:
                stubs[pc] = got_to_name[got]
    return stubs


def build(module):
    title = lab.title_id()
    nso = inventory.Nso(ROOT / f'local/runtime/user/dump/{title}/exefs/{module}')
    image = nso.flat()
    _, mod0 = inventory.parse_mod0(image)
    private = ROOT / f'local/analysis/{lab.target_name()}'
    funcs = sorted((int(json.loads(l)['offset'], 16), json.loads(l)['size'])
                   for l in (private / 'functions' / f'{module}.jsonl').read_text().splitlines() if l.strip())
    starts = [s for s, _ in funcs]
    names = {}
    for l in (private / 'symbols' / f'{module}.jsonl').read_text().splitlines():
        r = json.loads(l)
        if r['defined'] and r['symbol_type'] == 'func':
            names[int(r['offset'], 16)] = r['name']
    text_lo, text_hi = nso.text.vaddr, nso.text.end
    ro_lo, ro_hi = nso.rodata.vaddr, nso.rodata.end
    stubs = plt_imports(image, mod0, text_lo, text_hi)
    words = struct.unpack_from(f'<{(text_hi - text_lo) // 4}I', image, text_lo)

    def string_at(addr):
        if not (ro_lo <= addr < ro_hi):
            return None
        end = image.find(b'\0', addr, addr + 160)
        raw = image[addr:end] if end > addr else b''
        if len(raw) >= 3 and all(32 <= c < 127 or c in (9, 10) for c in raw):
            return raw.decode('ascii')
        return None

    callees = collections.defaultdict(collections.Counter)
    imports = collections.defaultdict(collections.Counter)
    strings = collections.defaultdict(set)
    data_refs = collections.defaultdict(set)
    adrp = {}
    fi = 0
    current = None
    for i, w in enumerate(words):
        pc = text_lo + i * 4
        while fi < len(funcs) and funcs[fi][0] <= pc:
            current = funcs[fi][0] if pc < funcs[fi][0] + funcs[fi][1] else None
            if pc >= funcs[fi][0] + funcs[fi][1]:
                fi += 1
                continue
            break
        if current is None:
            adrp.clear()
            continue
        top = w & 0xFC000000
        if top in (0x94000000, 0x14000000):          # BL / B
            target = pc + sxt(w & 0x3FFFFFF, 26) * 4
            if target in stubs:
                imports[current][stubs[target]] += 1
            elif top == 0x94000000 or not (current <= target < current + 0x100000 and
                                          bisect.bisect_right(starts, target) - 1 == bisect.bisect_right(starts, pc) - 1):
                j = bisect.bisect_right(starts, target) - 1
                if j >= 0 and starts[j] == target:
                    callees[current][target] += 1
        elif (w & 0x9F000000) == 0x90000000:          # ADRP
            adrp[w & 31] = (pc & ~0xFFF) + (sxt(((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3), 21) << 12)
        elif (w & 0xFF800000) == 0x91000000 and ((w >> 5) & 31) in adrp:   # ADD Xd, Xn, #imm
            addr = adrp[(w >> 5) & 31] + ((w >> 10) & 0xFFF) * (4096 if (w >> 22) & 1 else 1)
            text = string_at(addr)
            if text:
                strings[current].add(text)
            elif ro_lo <= addr < len(image):
                data_refs[current].add(addr)
        elif (w & 0xFFC00000) == 0xF9400000 and ((w >> 5) & 31) in adrp:   # LDR Xt, [Xn, #imm]
            data_refs[current].add(adrp[(w >> 5) & 31] + ((w >> 10) & 0xFFF) * 8)

    callers = collections.Counter()
    for f, cs in callees.items():
        for t in cs:
            callers[t] += 1
    out = index_dir() / f'{module}.jsonl'
    with out.open('w') as fh:
        for start, size in funcs:
            rec = {'offset': f'{start:#x}', 'size': size, 'name': names.get(start),
                   'callees': [f'{t:#x}' for t in sorted(callees[start])],
                   'imports': sorted(imports[start]), 'strings': sorted(strings[start])[:40],
                   'data_refs': len(data_refs[start]), 'callers': callers[start]}
            fh.write(json.dumps(rec) + '\n')
    stats = {'functions': len(funcs), 'plt_stubs': len(stubs),
             'call_edges': sum(len(c) for c in callees.values()),
             'functions_calling_imports': sum(1 for f in imports if imports[f]),
             'functions_with_strings': sum(1 for f in strings if strings[f])}
    (index_dir() / f'{module}.stats.json').write_text(json.dumps(stats, indent=1))
    print(module, stats)


def load(module):
    return [json.loads(l) for l in (index_dir() / f'{module}.jsonl').read_text().splitlines()]


def label(rec):
    return rec['name'] or f"sub_{int(rec['offset'], 16):x}"


def main():
    p = argparse.ArgumentParser()
    p.add_argument('command', choices=['build', 'string', 'import', 'func', 'top'])
    p.add_argument('arg', nargs='?')
    p.add_argument('--module', default='main')
    a = p.parse_args()
    if a.command == 'build':
        build(a.module)
        return 0
    recs = load(a.module)
    if a.command == 'string':
        hits = [r for r in recs if any(a.arg.lower() in s.lower() for s in r['strings'])]
        for r in hits[:60]:
            print(f"{label(r):<28} size {r['size']:>6} callers {r['callers']:>4}  " +
                  ' | '.join(s for s in r['strings'] if a.arg.lower() in s.lower())[:110])
        print(f'{len(hits)} function(s)')
    elif a.command == 'import':
        hits = [r for r in recs if any(a.arg in i for i in r['imports'])]
        for r in hits[:60]:
            print(f"{label(r):<28} size {r['size']:>6}  " + ', '.join(i for i in r['imports'] if a.arg in i)[:120])
        print(f'{len(hits)} function(s)')
    elif a.command == 'func':
        key = a.arg.lower()
        by_off = {r['offset']: r for r in recs}
        rec = by_off.get(key) or next((r for r in recs if r['name'] == a.arg or label(r) == a.arg), None)
        if not rec:
            print('not found'); return 1
        callers = [label(r) for r in recs if rec['offset'] in r['callees']]
        print(json.dumps({**rec, 'callees': [label(by_off[c]) if c in by_off else c for c in rec['callees']],
                          'caller_list': callers[:40]}, indent=1))
    elif a.command == 'top':
        n = int(a.arg or 20)
        for r in sorted(recs, key=lambda r: -r['callers'])[:n]:
            print(f"{label(r):<28} callers {r['callers']:>6} size {r['size']:>6} imports {len(r['imports'])}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
