#!/usr/bin/env python3
"""Name C++ classes from the game's own type-name strings, through their vtables.

The engine's reflected classes (data tables, parameters, systems) have a tiny virtual function that returns
the class name as a string, e.g. "ns__SomeTable" (namespaces joined by "__"); behaviour-graph
nodes do the same with a bare name ("SomeNodeQuery") from vtable slot 18. With the pointer
index (scripts/ptr-index.py) that gives, per class:
  <ns>::<Class>::vtable           the table itself (object)
  <ns>::<Class>::typeName         the function returning the name
  <ns>::<Class>::vf<N>            every slot function used by this class's vtable only
  <ns>::<Class>::ctor_dtor[_n]    code that stores the vtable address (constructors / destructors)
Functions shared by several vtables are base-class methods and stay unnamed here.

Inputs: local/analysis/<target>/index/main.{jsonl,ptrs.json}
Output: local/analysis/<target>/names/classes.jsonl (SymbolRecords, heuristic, with evidence)

  python3 scripts/name-classes.py [--module main]
"""
import argparse
import collections
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import lab  # noqa: E402

TYPE_NAME = re.compile(r'^[A-Za-z][A-Za-z0-9]*(?:__[A-Za-z0-9_]+)+$')
# Behaviour-graph node classes (Actions, Queries, Selectors, Decorators...) return a bare CamelCase name from
# slot 18 of vtables with 37+ slots; accepted only in that position, where it is consistent across ~2 400 classes.
NODE_NAME = re.compile(r'^[A-Z][A-Za-z0-9]{3,}$')
NODE_SLOT, NODE_MIN_SLOTS = 18, 37


# Slot roles shared by a whole node family (measured on this title: the per-class body sits in the same slot
# for 659 of 668 Actions and 665 of 698 Queries; slot 27 reads the node's parameters by name in both).
SLOT_ROLES = {'Action': {27: 'loadParams', 37: 'execute'}, 'Query': {27: 'loadParams', 28: 'evaluate'}}


def slot_role(name, size, slot):
    for family, roles in SLOT_ROLES.items():
        if name.endswith(family) and size >= NODE_MIN_SLOTS:
            return roles.get(slot)
    return None


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--module', default='main')
    a = p.parse_args()
    private = ROOT / f'local/analysis/{lab.target_name()}'
    recs = {json.loads(l)['offset']: json.loads(l) for l in (private / f'index/{a.module}.jsonl').read_text().splitlines()}
    tables = json.loads((private / f'index/{a.module}.ptrs.json').read_text())['tables']
    build_id = next((json.loads(l)['module']['build_id'] for l in (private / 'symbols' / f'{a.module}.jsonl')
                     .read_text().splitlines() if l.strip()), None)

    # Tiny functions (a few instructions) that reference exactly one type-like string.
    name_fn, node_fn = {}, {}
    for off, r in recs.items():
        if r['size'] <= 16:
            names = [s for s in r['strings'] if TYPE_NAME.match(s)]
            if len(names) == 1:
                name_fn[off] = names[0]
            nodes = [s for s in r['strings'] if NODE_NAME.match(s)]
            if len(nodes) == 1 and r['size'] <= 12:
                node_fn[off] = nodes[0]
    users = collections.Counter(f for t in tables.values() for f in set(t['slots']))
    classes = {}
    for addr, t in tables.items():
        found = {name_fn[f] for f in t['slots'] if f in name_fn}
        if len(found) == 1:
            classes[addr] = found.pop()
        elif not found and len(t['slots']) >= NODE_MIN_SLOTS and users[t['slots'][NODE_SLOT]] == 1 \
                and t['slots'][NODE_SLOT] in node_fn:
            classes[addr] = node_fn[t['slots'][NODE_SLOT]]
            name_fn[t['slots'][NODE_SLOT]] = classes[addr]

    out = []

    def record(offset, name, kind, size, note):
        out.append({'schema_version': 1, 'kind': 'SymbolRecord', 'module': {'name': a.module, 'build_id': build_id},
                    'offset': offset, 'size': size, 'name': name, 'symbol_type': kind, 'binding': 'unknown',
                    'defined': True, 'origin': 'heuristic',
                    'evidence': {'confidence': 'heuristic', 'source': 'scripts/name-classes.py', 'note': note}})

    taken = collections.Counter()
    installs = collections.Counter(i for addr in classes for i in tables[addr]['installers'])
    named_funcs = set()
    for addr, raw in sorted(classes.items()):
        cls = raw.replace('__', '::')
        t = tables[addr]
        record(addr, f'{cls}::vtable', 'object', len(t['slots']) * 8,
               f'vtable whose slot returns the type name "{raw}"')
        for i, f in enumerate(t['slots']):
            if f in named_funcs:
                continue
            if name_fn.get(f) == raw:
                record(f, f'{cls}::typeName', 'func', recs[f]['size'], f'returns "{raw}"')
            elif users[f] == 1:
                role = slot_role(raw, len(t['slots']), i)
                record(f, f'{cls}::{role or f"vf{i}"}', 'func', recs[f]['size'],
                       f'slot {i} of {cls}::vtable only' + (f'; role "{role}" from the node family layout' if role else ''))
            else:
                continue
            named_funcs.add(f)
        for f in t['installers']:
            if f in named_funcs or installs[f] != 1 or f not in recs:
                continue
            taken[cls] += 1
            suffix = '' if taken[cls] == 1 else f'_{taken[cls]}'
            record(f, f'{cls}::ctor_dtor{suffix}', 'func', recs[f]['size'],
                   f'stores the address of {cls}::vtable (constructor or destructor)')
            named_funcs.add(f)

    path = private / 'names' / 'classes.jsonl'
    path.write_text(''.join(json.dumps(r) + '\n' for r in out))
    kinds = collections.Counter(r['name'].rsplit('::', 1)[1].rstrip('_0123456789') for r in out)
    print(f'{len(classes)} classes; {len(out)} symbols ({dict(kinds)}) -> {path.relative_to(ROOT)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
