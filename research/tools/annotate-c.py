#!/usr/bin/env python3
"""Make Ghidra pseudo-C of a SWITCHPILER ELF readable: show strings and names it could not resolve.

Imported without auto-analysis, Ghidra prints a reference to a C string as &UNK_7102878fbc (or DAT_/s_), and
calls to functions named after the import as sub_<offset>. This rewrites, in place:
  &UNK_71xxxxxxxx  ->  &UNK_71xxxxxxxx/*"ColumnName"*/   when the address holds a printable C string
  sub_<offset>     ->  the best SWITCHPILER name for that offset, when one exists (names/*.jsonl)

  python3 scripts/annotate-c.py <dir-or-file.c>... [--base 0x7100000000]
"""
import argparse
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

REF = re.compile(r'&?(?:UNK|DAT|s|PTR)_(71[0-9a-f]{8})(?!/\*)')
SUB = re.compile(r'\bsub_([0-9a-f]+)\b')
MARK = '/*switchpiler*/'


def main():
    p = argparse.ArgumentParser()
    p.add_argument('paths', nargs='+', type=Path)
    p.add_argument('--base', default='0x7100000000')
    a = p.parse_args()
    base = int(a.base, 0)
    target = lab.target_name()
    image = inventory.Nso(ROOT / f'local/runtime/user/dump/{lab.title_id(target)}/exefs/main').flat()
    names = {}
    for f in ('data-readers.jsonl', 'classes.jsonl', 'manual.jsonl'):  # later files win
        path = ROOT / f'local/analysis/{target}/names/{f}'
        if path.exists():
            for line in path.read_text().splitlines():
                r = json.loads(line)
                if r['module']['name'] == 'main' and r['symbol_type'] == 'func':
                    names[int(r['offset'], 16)] = r['name'].replace('::', '__')

    def string_at(addr):
        off = addr - base
        if not 0 <= off < len(image):
            return None
        end = image.find(b'\0', off, off + 200)
        raw = image[off:end] if end > off else b''
        if len(raw) >= 2 and all(32 <= c < 127 for c in raw):
            return raw.decode().replace('*/', '* /')
        return None

    files = [f for p in a.paths for f in (sorted(p.glob('*.c')) if p.is_dir() else [p])]
    changed = 0
    for f in files:
        text = f.read_text()
        new = REF.sub(lambda m: m.group(0) + (f'/*"{s}"*/' if (s := string_at(int(m.group(1), 16))) else ''), text)
        new = SUB.sub(lambda m: names.get(int(m.group(1), 16), m.group(0)), new)
        if new != text:
            f.write_text(new)
            changed += 1
    print(f'annotated {changed}/{len(files)} file(s)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
