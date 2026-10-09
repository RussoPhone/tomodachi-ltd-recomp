#!/usr/bin/env python3
"""Recover C struct layouts of data tables from their decompiled reader functions.

A reader looks a column up by name, then stores the value into the object being filled:
    sub_5970(param_2, &UNK_7102878fbc);            // BYML lookup of a column (address of its name)
    *(int *)(param_1 + 0x28) = (int)extraout_x1;    // field at offset 0x28, type int
Matching the name (resolved from the address) with the next store to param_1 gives (column, offset, type).
For each table the reader with the most recovered fields wins; results become one header of structs.

Inputs:  local/decomp/<target>/c/readers/*.c (Ghidra pseudo-C), the main NSO (to resolve string addresses),
         local/analysis/<target>/data/rsdb-catalog.json (column names per table)
Outputs: local/decomp/<target>/types/rsdb_tables.h, local/decomp/<target>/types/rsdb_tables.json (private)
"""
import collections
import importlib.util
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import lab  # noqa: E402

spec = importlib.util.spec_from_file_location('inventory', ROOT / 'scripts/inventory-modules.py')
inventory = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inventory)

BASE = 0x7100000000
LOOKUP = re.compile(r'&(?:UNK|DAT|s)_([0-9a-f]{8,})')
STORE = re.compile(r'\*\((?P<type>[A-Za-z_0-9 ]+?\s*\**)\s*\)\s*\(param_1 \+ (?P<off>0x[0-9a-f]+|\d+)\)\s*=')
SUBOBJ = re.compile(r'param_1 \+ (?P<off>0x[0-9a-f]+|\d+)\s*,\s*&(?:UNK|DAT|s)_([0-9a-f]{8,})')
CTYPE = {'char *': 'const char*', 'undefined1': 'uint8_t', 'undefined2': 'uint16_t', 'char': 'int8_t',
         'int': 'int32_t', 'uint': 'uint32_t', 'bool': 'bool', 'float': 'float',
         'long': 'int64_t', 'ulong': 'uint64_t', 'short': 'int16_t', 'ushort': 'uint16_t', 'byte': 'uint8_t',
         'undefined8': 'uint64_t', 'undefined4': 'uint32_t', 'double': 'double'}


def main():
    target = lab.target_name()
    image = inventory.Nso(ROOT / f'local/runtime/user/dump/{lab.title_id(target)}/exefs/main').flat()

    def string_at(addr):
        off = addr - BASE
        if not (0 <= off < len(image)):
            return None
        end = image.find(b'\0', off, off + 128)
        raw = image[off:end] if end > off else b''
        return raw.decode('ascii') if raw and all(32 <= c < 127 for c in raw) else None

    catalog = {t['table']: set(t['column_names']) for t in
               json.loads((ROOT / f'local/analysis/{target}/data/rsdb-catalog.json').read_text())}
    readers = collections.defaultdict(list)
    for line in (ROOT / f'local/analysis/{target}/names/data-readers.jsonl').read_text().splitlines():
        r = json.loads(line)
        readers[int(r['offset'], 16)].append(r['name'].split('__')[0])

    best = {}
    for path in sorted((ROOT / f'local/decomp/{target}/c/readers').glob('*.c')):
        off = int(path.stem.rsplit('_', 1)[1], 16)
        tables = readers.get(off, [])
        lines = path.read_text().splitlines()
        fields = {}
        for i, line in enumerate(lines):
            sub = SUBOBJ.search(line)
            if sub:
                name = string_at(int(sub.group(2), 16))
                if name:
                    fields[int(sub.group('off'), 0)] = (name, 'struct/array (filled by callee)')
                continue
            m = LOOKUP.search(line)
            if not m:
                continue
            name = string_at(int(m.group(1), 16))
            if not name:
                continue
            for nxt in lines[i + 1:i + 8]:
                if LOOKUP.search(nxt):
                    break
                st = STORE.search(nxt)
                if st:
                    cast = st.group('type').strip()
                    pointee = cast[:-1].strip() if cast.endswith('*') else cast  # *(T *)(p+off) stores a T
                    ctype = CTYPE.get(pointee, pointee)
                    fields.setdefault(int(st.group('off'), 0), (name, ctype))
                    break
        for table in set(tables):
            cols = catalog.get(table, set())
            matched = {o: f for o, f in fields.items() if f[0] in cols or any(c.endswith(f[0]) for c in cols)}
            if len(matched) > len(best.get(table, (None, {}))[1]):
                best[table] = (off, matched)

    out = ROOT / f'local/decomp/{target}/types'
    out.mkdir(parents=True, exist_ok=True)
    header = ['// Recovered by SWITCHPILER scripts/recover-structs.py from reader functions (heuristic layouts:',
              '// offsets and types are what the reader stores; gaps are unknown). Not original source.',
              '#pragma once', '#include <stdint.h>', '#include <stdbool.h>', '']
    doc = {}
    for table, (reader, fields) in sorted(best.items()):
        cols = catalog.get(table, set())
        header.append(f'// {table}: {len(fields)}/{len(cols)} columns recovered, reader main+{reader:#x}')
        header.append(f'typedef struct {table} {{')
        cursor = 0
        used = collections.Counter()
        for offset in sorted(fields):
            name, ctype = fields[offset]
            used[name] += 1
            if used[name] > 1:
                name = f'{name}_{used[name]}'
            if offset > cursor:
                header.append(f'    uint8_t _unk_{cursor:#x}[{offset - cursor:#x}];')
            decl = f'/* {ctype} */ uint8_t {name}[8]' if ctype.startswith('struct') else f'{ctype} {name}'
            header.append(f'    {decl}; // +{offset:#x}')
            size = {'bool': 1, 'uint8_t': 1, 'int16_t': 2, 'uint16_t': 2, 'int32_t': 4, 'uint32_t': 4,
                    'float': 4}.get(ctype, 8)
            cursor = offset + size
        header.append(f'}} {table};\n')
        doc[table] = {'reader': f'{reader:#x}', 'columns': len(cols),
                      'fields': [{'offset': f'{o:#x}', 'name': n, 'type': t} for o, (n, t) in sorted(fields.items())]}
    (out / 'rsdb_tables.h').write_text('\n'.join(header) + '\n')
    (out / 'rsdb_tables.json').write_text(json.dumps(doc, indent=1))
    total = sum(len(v['fields']) for v in doc.values())
    print(f'{len(doc)} struct(s), {total} field(s) -> {(out / "rsdb_tables.h").relative_to(ROOT)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
