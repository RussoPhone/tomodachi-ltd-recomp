#!/usr/bin/env python3
"""Link data tables to the code that reads them, and propose names for those functions.

A table is read by column name, so a function that references several column names of the same RSDB table
is that table's reader/accessor. For every table, functions whose referenced strings cover >= MIN_COLUMNS of its
columns get a proposed name `<Table>__Reader` (or `__Reader_2`, ...), recorded as SymbolRecord origin=heuristic
with the evidence (how many and which columns matched).

Inputs:  local/analysis/<target>/data/rsdb-catalog.json, local/analysis/<target>/index/main.jsonl
Outputs: local/analysis/<target>/names/data-readers.jsonl (private), counts printed
"""
import collections
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import lab  # noqa: E402

MIN_COLUMNS = 3


def main():
    target = lab.target_name()
    private = ROOT / f'local/analysis/{target}'
    catalog = json.loads((private / 'data/rsdb-catalog.json').read_text())
    funcs = [json.loads(l) for l in (private / 'index/main.jsonl').read_text().splitlines()]
    module = json.loads((ROOT / f'adapters/{target}/modules/main.identity.json').read_text())['module']
    # a referenced string may be a shared suffix of the real one, so match columns as suffix-or-equal
    records, per_table = [], collections.Counter()
    for table in catalog:
        cols = [c for c in table['column_names'] if len(c) >= 5 and not c.startswith('__')]
        if len(cols) < MIN_COLUMNS:
            continue
        colset = set(cols)
        scored = []
        for f in funcs:
            hit = {c for s in f['strings'] for c in colset if s == c or (c.endswith(s) and len(s) >= 6)}
            if len(hit) >= MIN_COLUMNS:
                scored.append((len(hit), f, sorted(hit)))
        scored.sort(key=lambda x: -x[0])
        for rank, (n, f, hit) in enumerate(scored):
            name = f"{table['table']}__Reader" + (f'_{rank + 1}' if rank else '')
            records.append({'schema_version': 1, 'kind': 'SymbolRecord', 'module': module, 'offset': f['offset'],
                            'size': f['size'], 'name': name, 'symbol_type': 'func', 'binding': 'unknown',
                            'defined': True, 'origin': 'heuristic',
                            'evidence': {'confidence': 'heuristic',
                                         'source': f"references {n}/{len(cols)} column names of RSDB table {table['table']}",
                                         'note': ', '.join(hit[:12])}})
            per_table[table['table']] += 1
    out = private / 'names/data-readers.jsonl'
    out.write_text(''.join(json.dumps(r) + '\n' for r in records))
    print(f'{len(records)} function(s) linked to {len(per_table)} table(s) -> {out.relative_to(ROOT)}')
    for t, n in per_table.most_common(15):
        print(f'  {n:3d}  {t}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
