#!/usr/bin/env python3
"""Index and read the game's data files: zstd (.zs) -> SARC packs -> BYML tables, from the local install.

  python3 scripts/data-index.py build                 list every file inside every pack (and loose BYMLs)
  python3 scripts/data-index.py find <text>           files whose path contains <text>
  python3 scripts/data-index.py dump <path> [--out f] decode one BYML (inside a pack or loose) to JSON
  python3 scripts/data-index.py grep <text>           BYML files whose decoded content contains <text>

Reads local/install/<target>/romfs (the user's own decrypted data). Index and dumps stay in
local/analysis/<target>/data/ (private).
"""
import argparse
from compression import zstd
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import lab  # noqa: E402


def romfs():
    return ROOT / f'local/install/{lab.target_name()}/romfs'


def data_dir():
    d = ROOT / f'local/analysis/{lab.target_name()}/data'
    d.mkdir(parents=True, exist_ok=True)
    return d


def unzs(raw):
    return zstd.decompress(raw) if raw[:4] == b'\x28\xb5\x2f\xfd' else raw


def sarc_entries(blob):
    """Yield (name, bytes) for every file in a SARC archive."""
    if blob[:4] != b'SARC':
        raise ValueError('not SARC')
    bom = blob[6:8]
    e = '<' if bom == b'\xff\xfe' else '>'
    data_off = struct.unpack_from(e + 'I', blob, 0x0C)[0]
    sfat = struct.unpack_from(e + 'H', blob, 0x04)[0]
    assert blob[sfat:sfat + 4] == b'SFAT'
    count = struct.unpack_from(e + 'H', blob, sfat + 6)[0]
    nodes = sfat + 0x0C
    sfnt = nodes + count * 16
    assert blob[sfnt:sfnt + 4] == b'SFNT'
    names = sfnt + 8
    for i in range(count):
        _, attr, start, end = struct.unpack_from(e + 'IIII', blob, nodes + i * 16)
        name = f'#{i}'
        if attr & 0x01000000:
            off = names + (attr & 0xFFFF) * 4
            name = blob[off:blob.index(b'\0', off)].decode('utf-8', 'replace')
        yield name, blob[data_off + start:data_off + end]


class Byml:
    def __init__(self, blob):
        if blob[:2] == b'YB':
            self.e = '<'
        elif blob[:2] == b'BY':
            self.e = '>'
        else:
            raise ValueError('not BYML')
        self.b = blob
        self.version = struct.unpack_from(self.e + 'H', blob, 2)[0]
        hk, st, root = struct.unpack_from(self.e + 'III', blob, 4)
        self.keys = self._strings(hk) if hk else []
        self.strs = self._strings(st) if st else []
        self.root_off = root

    def u32(self, o):
        return struct.unpack_from(self.e + 'I', self.b, o)[0]

    def _strings(self, off):
        assert self.b[off] == 0xC2
        n = self.u32(off) >> 8 if self.e == '<' else self.u32(off) & 0xFFFFFF
        out = []
        for i in range(n):
            s = off + self.u32(off + 4 + i * 4)
            out.append(self.b[s:self.b.index(b'\0', s)].decode('utf-8', 'replace'))
        return out

    def header(self, off):
        word = self.u32(off)
        return (word & 0xFF, word >> 8) if self.e == '<' else (word >> 24, word & 0xFFFFFF)

    def value(self, kind, raw):
        e = self.e
        if kind == 0xA0:
            return self.strs[raw]
        if kind == 0xD0:
            return bool(raw)
        if kind == 0xD1:
            return struct.unpack(e + 'i', struct.pack(e + 'I', raw))[0]
        if kind == 0xD2:
            return round(struct.unpack(e + 'f', struct.pack(e + 'I', raw))[0], 6)
        if kind == 0xD3:
            return raw
        if kind in (0xD4, 0xD5, 0xD6):
            v = struct.unpack_from(e + {0xD4: 'q', 0xD5: 'Q', 0xD6: 'd'}[kind], self.b, raw)[0]
            return round(v, 9) if kind == 0xD6 else v
        if kind == 0xFF:
            return None
        if kind in (0xC0, 0xC1, 0x20, 0x21):
            return self.node(raw)
        if kind == 0xA1:
            size = self.u32(raw)
            return {'_binary_bytes': size}
        return {'_unknown_type': hex(kind), '_raw': raw}

    def node(self, off):
        kind, n = self.header(off)
        if kind == 0xC0:  # array: n type bytes (4-aligned), then n u32 values
            types = self.b[off + 4:off + 4 + n]
            values = off + 4 + ((n + 3) & ~3)
            return [self.value(types[i], self.u32(values + i * 4)) for i in range(n)]
        if kind == 0xC1:  # dictionary: n entries of (u24 key index + u8 type, u32 value)
            out = {}
            for i in range(n):
                ent = off + 4 + i * 8
                w = self.u32(ent)
                key, t = (w & 0xFFFFFF, w >> 24) if self.e == '<' else (w >> 8, w & 0xFF)
                out[self.keys[key]] = self.value(t, self.u32(ent + 4))
            return out
        if kind in (0x20, 0x21):  # hash maps (v7): n entries of (u32/u64 hash, u32 value) + type bytes
            ksize = 4 if kind == 0x20 else 8
            ent = 8 if kind == 0x20 else 12
            types = off + 4 + n * ent
            out = {}
            for i in range(n):
                p = off + 4 + i * ent
                h = struct.unpack_from(self.e + ('I' if ksize == 4 else 'Q'), self.b, p)[0]
                out[f'{h:#x}'] = self.value(self.b[types + i], self.u32(p + ksize))
            return out
        return self.value(kind, off)

    def root(self):
        return self.node(self.root_off) if self.root_off else None


def iter_files():
    """Yield (container, inner_path, bytes) for loose files and every SARC member."""
    base = romfs()
    for path in sorted(base.rglob('*')):
        if not path.is_file():
            continue
        rel = '/' + str(path.relative_to(base))
        name = rel[:-3] if rel.endswith('.zs') else rel
        if name.endswith(('.pack', '.sarc')):
            try:
                blob = unzs(path.read_bytes())
                for inner, data in sarc_entries(blob):
                    yield rel, inner, data
            except Exception as error:  # report, keep going
                yield rel, f'!error {error}', b''
        elif name.endswith(('.byml', '.bgyml')):
            yield rel, '', unzs(path.read_bytes())


def cmd_build():
    out = data_dir() / 'files.tsv'
    n = errors = 0
    with out.open('w') as fh:
        fh.write('container\tinner\tsize\tmagic\n')
        for container, inner, data in iter_files():
            if inner.startswith('!error'):
                errors += 1
            fh.write(f'{container}\t{inner}\t{len(data)}\t{data[:4].hex()}\n')
            n += 1
    print(f'{n} entries indexed ({errors} unreadable packs) -> {out.relative_to(ROOT)}')


def rows():
    return [l.rstrip('\n').split('\t') for l in (data_dir() / 'files.tsv').read_text().splitlines()[1:]]


def load(container, inner):
    blob = unzs((romfs() / container.lstrip('/')).read_bytes())
    if not inner:
        return blob
    return next(d for n, d in sarc_entries(blob) if n == inner)


def cmd_find(text):
    hits = [r for r in rows() if text.lower() in (r[0] + '/' + r[1]).lower()]
    for r in hits[:80]:
        print(f'{r[2]:>8}  {r[0]}  ::  {r[1]}')
    print(f'{len(hits)} file(s)')


def resolve(path):
    for r in rows():
        if r[1] == path or r[0] == path or (r[1] and r[1].endswith(path)):
            return r
    raise SystemExit(f'not found: {path}')


def cmd_dump(path, out):
    r = resolve(path)
    doc = Byml(load(r[0], r[1])).root()
    text = json.dumps(doc, indent=1, ensure_ascii=False)
    if out:
        Path(out).write_text(text)
        print(f'written {out}')
    else:
        print(text[:6000])


def cmd_grep(text):
    hits = 0
    for r in rows():
        if not r[1].endswith(('.byml', '.bgyml')) and not r[0].endswith(('.byml', '.bgyml', '.byml.zs', '.bgyml.zs')):
            continue
        try:
            doc = json.dumps(Byml(load(r[0], r[1])).root(), ensure_ascii=False)
        except Exception:
            continue
        if text.lower() in doc.lower():
            hits += 1
            if hits <= 40:
                print(f'{r[0]} :: {r[1]}')
    print(f'{hits} file(s)')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('command', choices=['build', 'find', 'dump', 'grep'])
    p.add_argument('arg', nargs='?')
    p.add_argument('--out')
    a = p.parse_args()
    if a.command == 'build':
        cmd_build()
    elif a.command == 'find':
        cmd_find(a.arg)
    elif a.command == 'dump':
        cmd_dump(a.arg, a.out)
    else:
        cmd_grep(a.arg)
    return 0


if __name__ == '__main__':
    sys.exit(main())
