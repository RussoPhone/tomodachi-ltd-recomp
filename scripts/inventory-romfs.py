#!/usr/bin/env python3
"""Inventory every RomFS in the configured NSP without extracting it.

Reads the PFS0 file table, decrypts each NCA header (XTS, header_key) and the RomFS sections
(AES-CTR, key area decrypted with key_area_key_application_<gen>) in memory, walks the RomFS
directory/file tables and samples the first bytes of each file to classify its format.

Outputs:
  local/analysis/<target>/romfs/<nca-content-type>.tsv   path, size, magic (private: names are content)
  adapters/<target>/modules/analysis-romfs.json           counts and bytes per format (public)

Keys are read from the portable profile and never printed. The dump is opened read-only.

  python3 scripts/inventory-romfs.py
"""
import collections
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import lab  # noqa: E402

CONTENT_TYPES = {0: 'program', 1: 'meta', 2: 'control', 3: 'manual', 4: 'data', 5: 'publicdata'}
MAGICS = [(b'\x28\xb5\x2f\xfd', 'zstd'), (b'Yaz0', 'yaz0'), (b'SARC', 'sarc'), (b'FRES', 'bfres'),
          (b'BNTX', 'bntx'), (b'MsgStdBn', 'msbt'), (b'MsgPrjBn', 'msbp'), (b'BY', 'byml'), (b'YB', 'byml'),
          (b'FWAV', 'bfwav'), (b'FSTM', 'bfstm'), (b'BWAV', 'bwav'), (b'BARS', 'bars'), (b'FLYT', 'bflyt'),
          (b'FLAN', 'bflan'), (b'FLIM', 'bflim'), (b'FFNT', 'bffnt'), (b'AAMP', 'aamp'), (b'EFTB', 'eft'),
          (b'VFXB', 'ptcl'), (b'BFSAR', 'bfsar'), (b'FSAR', 'bfsar'), (b'\x89PNG', 'png'), (b'RIFF', 'riff'),
          (b'PK\x03\x04', 'zip'), (b'NARC', 'narc'), (b'BCSV', 'bcsv'), (b'\x7fELF', 'elf'), (b'NRO0', 'nro'),
          (b'MOD0', 'mod0'), (b'RSTB', 'rstb'), (b'RESTBL', 'restbl'), (b'Gfx2', 'gtx'), (b'MSARC', 'msarc'),
          (b'{', 'json?'), (b'<', 'xml?'), (b'\xef\xbb\xbf', 'utf8-bom text')]


def keys():
    text = (ROOT / 'local/runtime/user/keys/prod.keys').read_text()
    table = {}
    for name, value in re.findall(r'^([a-z0-9_]+)\s*=\s*([0-9a-fA-F]+)\s*$', text, re.M):
        table[name] = bytes.fromhex(value)
    return table


def pfs0(f):
    f.seek(0)
    magic, count, strtab_size, _ = struct.unpack('<4sIII', f.read(16))
    if magic != b'PFS0':
        raise ValueError('not a PFS0 container')
    entries = [struct.unpack('<QQI4x', f.read(24)) for _ in range(count)]
    strtab = f.read(strtab_size)
    base = 16 + 24 * count + strtab_size
    return [(strtab[n:strtab.index(b'\0', n)].decode(), base + off, size) for off, size, n in entries]


def xts_decrypt(key, data, sector_size=0x200):
    out = bytearray()
    for i in range(0, len(data), sector_size):
        decryptor = Cipher(algorithms.AES(key), modes.XTS((i // sector_size).to_bytes(16, 'big'))).decryptor()
        out += decryptor.update(data[i:i + sector_size]) + decryptor.finalize()
    return bytes(out)


class CtrReader:
    """Random access to an AES-CTR section of an NCA inside the NSP."""

    def __init__(self, f, nca_offset, key, section_ctr):
        self.f, self.nca_offset, self.key, self.upper = f, nca_offset, key, section_ctr[::-1]

    def read(self, offset, size):
        start = offset & ~0xF
        self.f.seek(self.nca_offset + start)
        raw = self.f.read((offset - start + size + 0xF) & ~0xF)
        counter = self.upper + (start >> 4).to_bytes(8, 'big')
        decryptor = Cipher(algorithms.AES(self.key), modes.CTR(counter)).decryptor()
        plain = decryptor.update(raw) + decryptor.finalize()
        return plain[offset - start:offset - start + size]


def classify(head):
    for magic, name in MAGICS:
        if head.startswith(magic):
            return name
    if head[:4].isascii() and head[:4].isalnum():
        return 'magic:' + head[:4].decode()
    return 'unknown'


def walk_romfs(reader, base):
    header = struct.unpack('<11Q', reader.read(base, 0x58))
    header_size, _, _, dir_meta_off, dir_meta_size, _, _, file_meta_off, file_meta_size, data_off = header[:10]
    if header_size != 0x50:
        raise ValueError(f'RomFS header size {header_size:#x}: decryption or layout is wrong')
    dir_meta = reader.read(base + dir_meta_off, dir_meta_size)
    file_meta = reader.read(base + file_meta_off, file_meta_size)

    def dir_name(offset):
        parent, _, _, _, _, name_len = struct.unpack_from('<6I', dir_meta, offset)
        name = dir_meta[offset + 24:offset + 24 + name_len].decode('utf-8', 'replace')
        return (dir_name(parent) + '/' + name) if offset != 0 else ''

    files = []
    offset = 0
    while offset < len(file_meta):
        parent, _, data, size, _, name_len = struct.unpack_from('<IIQQII', file_meta, offset)
        name = file_meta[offset + 32:offset + 32 + name_len].decode('utf-8', 'replace')
        files.append((dir_name(parent) + '/' + name, base + data_off + data, size))
        offset += (32 + name_len + 3) & ~3
    return files


def main():
    target = lab.target_name()
    key_table = keys()
    header_key = key_table['header_key']
    private = ROOT / f'local/analysis/{target}/romfs'
    private.mkdir(parents=True, exist_ok=True)
    dump = lab.runtime()['dump_path']
    summary, outputs = {}, []
    with open(dump, 'rb') as f:
        for name, nca_offset, _ in pfs0(f):
            if not name.endswith('.nca'):
                continue
            f.seek(nca_offset)
            header = xts_decrypt(header_key, f.read(0xC00))
            if header[0x200:0x204] != b'NCA3':
                raise SystemExit(f'{name}: header does not decrypt')
            content = CONTENT_TYPES.get(header[0x205], str(header[0x205]))
            if any(header[0x230:0x240]):
                summary[content] = {'skipped': 'titlekey crypto not handled here'}
                continue
            generation = max(header[0x206], header[0x220])
            generation = generation - 1 if generation else 0
            kak = key_table[f'key_area_key_application_{generation:02x}']
            area = Cipher(algorithms.AES(kak), modes.ECB()).decryptor().update(header[0x300:0x340])
            ctr_key = area[0x20:0x30]
            for section in range(4):
                start, end = struct.unpack_from('<II', header, 0x240 + section * 0x10)
                if end <= start:
                    continue
                fs = header[0x400 + section * 0x200:0x600 + section * 0x200]
                hash_type, encryption = fs[3], fs[4]
                if hash_type != 3 or encryption != 3:  # IVFC (RomFS) with plain AES-CTR only
                    continue
                if fs[0x8:0xC] != b'IVFC':
                    raise SystemExit(f'{name} section {section}: no IVFC header')
                level_offset = struct.unpack_from('<Q', fs, 0x8 + 0x10 + 5 * 0x18)[0]
                reader = CtrReader(f, nca_offset, ctr_key, fs[0x140:0x148])
                files = walk_romfs(reader, start * 0x200 + level_offset)
                rows, by_format = [], collections.defaultdict(lambda: [0, 0])
                for path, offset, size in files:
                    fmt = classify(reader.read(offset, min(size, 16))) if size else 'empty'
                    by_format[fmt][0] += 1
                    by_format[fmt][1] += size
                    rows.append(f'{path}\t{size}\t{fmt}\n')
                out = private / f'{content}.tsv'
                out.write_text('path\tsize\tformat\n' + ''.join(rows))
                outputs.append({'path': str(out.relative_to(ROOT)), 'sha256': hashlib.sha256(out.read_bytes()).hexdigest(),
                                'private': True})
                extensions = collections.Counter(Path(p).suffix.lower() or '(none)' for p, _, _ in files)
                inner = collections.defaultdict(lambda: [0, 0])  # x.bfres.zs -> .bfres (name-based)
                for path, _, size in files:
                    if path.endswith('.zs'):
                        stem = Path(path[:-3])
                        inner[stem.suffix.lower() or '(none)'][0] += 1
                        inner[stem.suffix.lower() or '(none)'][1] += size
                summary[content] = {'files': len(files), 'bytes': sum(s for _, _, s in files),
                                    'formats': {k: {'files': v[0], 'bytes': v[1]}
                                                for k, v in sorted(by_format.items(), key=lambda kv: -kv[1][1])},
                                    'top_extensions': dict(extensions.most_common(15)),
                                    'zstd_inner_extensions': {k: {'files': v[0], 'compressed_bytes': v[1]}
                                                              for k, v in sorted(inner.items(), key=lambda kv: -kv[1][1])[:25]}}
                print(f'{content}: {len(files)} files, {summary[content]["bytes"] / 2**30:.2f} GiB')
    identity = json.loads((ROOT / 'artifacts/input-identity.json').read_text())
    artifact = {'schema_version': 1, 'kind': 'AnalysisArtifact', 'analysis': 'romfs-inventory',
                'subject': {'title_id': lab.title_id(target)},
                'producer': {'tool': 'scripts/inventory-romfs.py', 'revision': __import__('subprocess').run(
                    ['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()},
                'inputs': [{'role': 'nsp', 'sha256': identity['sha256']}],
                'outputs': outputs, 'status': 'passed', 'results': summary,
                'interpretation': [
                    {'claim': 'Formats come from the first bytes of each file; compressed files (zstd, yaz0) hide their inner format.',
                     'evidence': {'confidence': 'heuristic', 'source': 'magic-number sampling'}}]}
    (ROOT / f'adapters/{target}/modules/analysis-romfs.json').write_text(json.dumps(artifact, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
