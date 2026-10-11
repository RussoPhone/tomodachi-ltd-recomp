#!/usr/bin/env python3
"""Inventory every RomFS in the configured NSP without extracting it.

Reads the PFS0 file table, decrypts each NCA header (XTS, header_key) and the RomFS sections
(AES-CTR, key area decrypted with key_area_key_application_<gen>) in memory, walks the RomFS
directory/file tables and samples the first bytes of each file to classify its format.

Outputs:
  local/analysis/<target>/romfs/<nca-content-type>[-<number>].tsv   path, size, magic (private: names are content)
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
    file_size = f.seek(0, 2)
    f.seek(0)
    fixed = f.read(16)
    if len(fixed) != 16:
        raise ValueError('truncated PFS0 header')
    magic, count, strtab_size, _ = struct.unpack('<4sIII', fixed)
    if magic != b'PFS0':
        raise ValueError('not a PFS0 container')
    table_end = 16 + count * 24 + strtab_size
    if table_end > file_size:
        raise ValueError('PFS0 file table or string table exceeds container size')
    entries = []
    for _ in range(count):
        raw = f.read(24)
        if len(raw) != 24:
            raise ValueError('truncated PFS0 file entry')
        entries.append(struct.unpack('<QQI4x', raw))
    strtab = f.read(strtab_size)
    base = 16 + 24 * count + strtab_size
    if len(strtab) != strtab_size:
        raise ValueError('truncated PFS0 string table')
    files = []
    for off, size, name_offset in entries:
        if name_offset >= len(strtab):
            raise ValueError(f'PFS0 name offset {name_offset:#x} is outside string table')
        name_end = strtab.find(b'\0', name_offset)
        if name_end < 0:
            raise ValueError(f'PFS0 name at {name_offset:#x} is not terminated')
        try:
            name = strtab[name_offset:name_end].decode('utf-8')
        except UnicodeDecodeError as exc:
            raise ValueError(f'PFS0 name at {name_offset:#x} is not UTF-8') from exc
        if off > file_size - base or size > file_size - base - off:
            raise ValueError(f'PFS0 entry {name!r} exceeds container size')
        files.append((name, base + off, size))
    return files


def xts_decrypt(key, data, sector_size=0x200):
    out = bytearray()
    for i in range(0, len(data), sector_size):
        decryptor = Cipher(algorithms.AES(key), modes.XTS((i // sector_size).to_bytes(16, 'big'))).decryptor()
        out += decryptor.update(data[i:i + sector_size]) + decryptor.finalize()
    return bytes(out)


class CtrReader:
    """Random access to an AES-CTR section of an NCA inside the NSP."""

    def __init__(self, f, nca_offset, key, section_ctr, nca_size=None,
                 section_offset=None, section_size=None):
        self.f, self.nca_offset, self.key, self.upper = f, nca_offset, key, section_ctr[::-1]
        self.nca_size = nca_size
        self.section_start = section_offset
        self.section_end = section_offset + section_size if section_offset is not None else None
        if section_offset is not None and (section_offset < 0 or section_size is None or section_size < 0):
            raise ValueError('invalid AES-CTR section extent')
        if nca_size is not None and (nca_size < 0 or
                                     (self.section_end is not None and self.section_end > nca_size)):
            raise ValueError('AES-CTR section exceeds NCA extent')

    def check_range(self, offset, size):
        if offset < 0 or size < 0:
            raise ValueError('negative AES-CTR read range')
        end = offset + size
        if self.nca_size is not None and end > self.nca_size:
            raise ValueError('AES-CTR read exceeds NCA extent')
        if self.section_start is not None and (offset < self.section_start or end > self.section_end):
            raise ValueError('AES-CTR read exceeds section extent')

    def read(self, offset, size):
        self.check_range(offset, size)
        start = offset & ~0xF
        raw_size = (offset - start + size + 0xF) & ~0xF
        if self.section_start is not None and start + raw_size > self.section_end:
            raise ValueError('aligned AES-CTR read exceeds section extent')
        if self.nca_size is not None and start + raw_size > self.nca_size:
            raise ValueError('aligned AES-CTR read exceeds NCA extent')
        self.f.seek(self.nca_offset + start)
        raw = self.f.read(raw_size)
        if len(raw) != raw_size:
            raise EOFError(f'short AES-CTR read at {start:#x}: expected {raw_size:#x}, got {len(raw):#x}')
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


def read_nca_header(f, nca_offset, header_key, name):
    f.seek(nca_offset)
    encrypted = f.read(0xC00)
    if len(encrypted) != 0xC00:
        raise ValueError(f'{name}: truncated NCA header')
    return xts_decrypt(header_key, encrypted)


def walk_romfs(reader, base):
    header_data = reader.read(base, 0x58)
    if len(header_data) != 0x58:
        raise EOFError('truncated RomFS header')
    header = struct.unpack('<11Q', header_data)
    header_size, _, _, dir_meta_off, dir_meta_size, _, _, file_meta_off, file_meta_size, data_off = header[:10]
    if header_size != 0x50:
        raise ValueError(f'RomFS header size {header_size:#x}: decryption or layout is wrong')
    _check_reader_range(reader, base + dir_meta_off, dir_meta_size)
    _check_reader_range(reader, base + file_meta_off, file_meta_size)
    dir_meta = reader.read(base + dir_meta_off, dir_meta_size)
    file_meta = reader.read(base + file_meta_off, file_meta_size)
    if len(dir_meta) != dir_meta_size or len(file_meta) != file_meta_size:
        raise EOFError('truncated RomFS metadata table')

    def dir_name(offset, visiting=frozenset()):
        if offset == 0:
            return ''
        if offset % 4 or offset < 0 or offset + 24 > len(dir_meta):
            raise ValueError(f'RomFS directory metadata offset {offset:#x} is out of bounds')
        if offset in visiting:
            raise ValueError(f'RomFS directory parent cycle at offset {offset:#x}')
        parent, _, _, _, _, name_len = struct.unpack_from('<6I', dir_meta, offset)
        name_end = offset + 24 + name_len
        if name_end > len(dir_meta):
            raise ValueError(f'RomFS directory name at {offset:#x} exceeds metadata table')
        name = dir_meta[offset + 24:name_end].decode('utf-8', 'replace')
        prefix = dir_name(parent, visiting | {offset})
        return (prefix + '/' + name) if prefix else '/' + name

    files = []
    offset = 0
    while offset < len(file_meta):
        if offset + 32 > len(file_meta):
            raise ValueError(f'truncated RomFS file metadata at {offset:#x}')
        parent, _, data, size, _, name_len = struct.unpack_from('<IIQQII', file_meta, offset)
        name_end = offset + 32 + name_len
        if name_end > len(file_meta):
            raise ValueError(f'RomFS file name at {offset:#x} exceeds metadata table')
        name = file_meta[offset + 32:name_end].decode('utf-8', 'replace')
        file_offset = base + data_off + data
        _check_reader_range(reader, file_offset, size)
        prefix = dir_name(parent)
        files.append(((prefix + '/' if prefix else '/') + name, file_offset, size))
        next_offset = (32 + name_len + 3) & ~3
        if next_offset <= 0 or offset + next_offset > len(file_meta):
            raise ValueError(f'RomFS file record at {offset:#x} has invalid size')
        offset += next_offset
    return files


def _check_reader_range(reader, offset, size):
    if offset < 0 or size < 0:
        raise ValueError('negative RomFS read range')
    check = getattr(reader, 'check_range', None)
    if check is not None:
        check(offset, size)
    elif hasattr(reader, 'length') and offset + size > reader.length:
        raise ValueError('RomFS read exceeds reader extent')


def safe_output_path(path):
    root = ROOT.resolve()
    resolved = path.resolve()
    if resolved != root and root not in resolved.parents:
        raise ValueError(f'analysis output escapes repository root: {path}')
    if path.is_symlink():
        raise ValueError(f'analysis output must not be a symlink: {path}')
    return path


def tsv_escape(value):
    return value.replace('\\', '\\\\').replace('\t', '\\t').replace('\r', '\\r').replace('\n', '\\n')


def main():
    target = lab.target_name()
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', target) or target in {'.', '..'}:
        raise ValueError(f'invalid target name for analysis output: {target!r}')
    key_table = keys()
    header_key = key_table['header_key']
    private = safe_output_path(ROOT / f'local/analysis/{target}/romfs')
    private.mkdir(parents=True, exist_ok=True)
    dump = lab.runtime()['dump_path']
    summary, outputs = {}, []
    content_occurrences = collections.Counter()
    with open(dump, 'rb') as f:
        for name, nca_offset, nca_size in pfs0(f):
            if not name.endswith('.nca'):
                continue
            header = read_nca_header(f, nca_offset, header_key, name)
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
                reader = CtrReader(f, nca_offset, ctr_key, fs[0x140:0x148], nca_size,
                                   start * 0x200, (end - start) * 0x200)
                files = walk_romfs(reader, start * 0x200 + level_offset)
                content_occurrences[content] += 1
                label = content if content_occurrences[content] == 1 else f'{content}-{content_occurrences[content]}'
                rows, by_format = [], collections.defaultdict(lambda: [0, 0])
                for path, offset, size in files:
                    fmt = classify(reader.read(offset, min(size, 16))) if size else 'empty'
                    by_format[fmt][0] += 1
                    by_format[fmt][1] += size
                    rows.append(f'{tsv_escape(path)}\t{size}\t{fmt}\n')
                out = safe_output_path(private / f'{label}.tsv')
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
                summary[label] = {'files': len(files), 'bytes': sum(s for _, _, s in files),
                                    'formats': {k: {'files': v[0], 'bytes': v[1]}
                                                for k, v in sorted(by_format.items(), key=lambda kv: -kv[1][1])},
                                    'top_extensions': dict(extensions.most_common(15)),
                                    'zstd_inner_extensions': {k: {'files': v[0], 'compressed_bytes': v[1]}
                                                              for k, v in sorted(inner.items(), key=lambda kv: -kv[1][1])[:25]}}
                print(f'{label}: {len(files)} files, {summary[label]["bytes"] / 2**30:.2f} GiB')
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
    public_artifact = safe_output_path(ROOT / f'adapters/{target}/modules/analysis-romfs.json')
    public_artifact.parent.mkdir(parents=True, exist_ok=True)
    public_artifact.write_text(json.dumps(artifact, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
