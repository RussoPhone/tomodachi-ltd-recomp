#!/usr/bin/env python3
"""Turn a suyu ExeFS dump into ModuleIdentity, SegmentMap and SymbolRecord documents.

Input is the directory suyu writes with dump_exefs=true (user/dump/<title>/exefs).
Public metadata (identities, segment maps, analysis summaries) goes to
adapters/<target>/modules/. Symbol names come from the game binary and stay in
local/analysis/<target>/; only their counts are published.

  python3 scripts/inventory-modules.py --target tomodachi --exefs <dir> \
      [--decode-tsv <decode.tsv>]
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'upstream/mk8-recomp/scripts'))
from nso import Nso  # noqa: E402  (upstream parser, pinned in upstream.lock.json)

LOCK = json.loads((ROOT / 'upstream.lock.json').read_text())
PERMISSIONS = {'text': 'r-x', 'rodata': 'r--', 'data': 'rw-', 'bss': 'rw-'}
SYMBOL_TYPES = {0: 'notype', 1: 'object', 2: 'func', 3: 'section', 4: 'file', 6: 'tls'}
BINDINGS = {0: 'local', 1: 'global', 2: 'weak'}
DT_NULL, DT_HASH, DT_STRTAB, DT_SYMTAB, DT_STRSZ, DT_SYMENT = 0, 4, 5, 6, 10, 11


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def hexo(value):
    return f'{value:#x}'


def lab_revision():
    head = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'],
                          capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(['git', '-C', str(ROOT), 'status', '--porcelain', '--', 'scripts', 'schemas'],
                           capture_output=True, text=True).stdout.strip()
    return head + ('+dirty' if dirty else '')


def producer(tool, **parameters):
    record = {'tool': tool, 'revision': lab_revision(), 'backend_revision': LOCK['commit']}
    if parameters:
        record['parameters'] = parameters
    return record


def parse_npdm(blob):
    if blob[:4] != b'META':
        raise ValueError('main.npdm: bad magic')
    flags = blob[0x0C]
    return {'is_64bit': bool(flags & 1), 'address_space_type': (flags >> 1) & 7, 'flags': flags}


def parse_mod0(image):
    mod0 = struct.unpack_from('<I', image, 4)[0]
    if image[mod0:mod0 + 4] != b'MOD0':
        return None
    rel = struct.unpack_from('<iiiiii', image, mod0 + 4)
    names = ('dynamic', 'bss_start', 'bss_end', 'eh_frame_hdr_start', 'eh_frame_hdr_end', 'module_object')
    return mod0, {name: mod0 + value for name, value in zip(names, rel)}


def dynamic_symbols(image, dynamic):
    tags = {}
    offset = dynamic
    while offset + 16 <= len(image):
        tag, value = struct.unpack_from('<qQ', image, offset)
        if tag == DT_NULL:
            break
        tags.setdefault(tag, value)
        offset += 16
    if DT_SYMTAB not in tags or DT_STRTAB not in tags:
        return []
    symtab, strtab = tags[DT_SYMTAB], tags[DT_STRTAB]
    entsize = tags.get(DT_SYMENT, 24)
    if DT_HASH in tags:
        count = struct.unpack_from('<I', image, tags[DT_HASH] + 4)[0]
    elif strtab > symtab:
        count = (strtab - symtab) // entsize
    else:
        return []
    symbols = []
    for index in range(1, count):
        name_off, info, _, shndx, value, size = struct.unpack_from('<IBBHQQ', image, symtab + index * entsize)
        end = image.index(b'\0', strtab + name_off)
        name = image[strtab + name_off:end].decode('utf-8', 'replace')
        if name:
            symbols.append({'name': name, 'value': value, 'size': size, 'defined': shndx != 0,
                            'type': SYMBOL_TYPES.get(info & 0xF, 'unknown'),
                            'binding': BINDINGS.get(info >> 4, 'unknown')})
    return symbols


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--target', required=True)
    parser.add_argument('--exefs', required=True, type=Path)
    parser.add_argument('--decode-tsv', type=Path)
    args = parser.parse_args()

    adapter = json.loads((ROOT / f'adapters/{args.target}/target.json').read_text())
    identity = json.loads((ROOT / 'artifacts/input-identity.json').read_text())
    public = ROOT / f'adapters/{args.target}/modules'
    private = ROOT / f'local/analysis/{args.target}'
    public.mkdir(parents=True, exist_ok=True)
    (private / 'symbols').mkdir(parents=True, exist_ok=True)

    npdm_blob = (args.exefs / 'main.npdm').read_bytes()
    npdm = parse_npdm(npdm_blob)
    isa_evidence = {'confidence': 'fact', 'source': f'main.npdm flags={npdm["flags"]:#04x} (sha256 {sha256(npdm_blob)[:16]}…)',
                    'note': 'process-wide: every module in this ExeFS runs in the same AArch64 process'}

    names = sorted(p.name for p in args.exefs.iterdir() if p.is_file() and p.name != 'main.npdm')
    refs, symbol_summary = [], {}
    for name in names:
        path = args.exefs / name
        blob = path.read_bytes()
        nso = Nso(path)  # verifies per-segment SHA-256 when the NSO carries them
        image = nso.flat()
        ref = {'name': name, 'build_id': nso.build_id}
        refs.append(ref)
        flags = struct.unpack_from('<I', blob, 0x0C)[0]

        module_identity = {
            'schema_version': 1, 'kind': 'ModuleIdentity', 'module': ref, 'format': 'NSO0',
            'architecture': {'isa': 'AArch64' if npdm['is_64bit'] else 'AArch32', 'evidence': isa_evidence},
            'hashes': {'file_sha256': sha256(blob), 'image_sha256': sha256(image)},
            'origin': {'title_id': adapter['expected_title_id'], 'content_type': 'base', 'content_version': 0,
                       'container_sha256': identity['sha256'], 'exefs_name': name},
            'producer': producer('scripts/inventory-modules.py', source='suyu dump_exefs'),
        }

        segments = []
        for index, seg in enumerate(nso.segments):
            file_off, _, _ = struct.unpack_from('<III', blob, 0x10 + index * 0x10)
            csize = struct.unpack_from('<I', blob, 0x60 + index * 4)[0]
            segments.append({'name': seg.name, 'file_offset': hexo(file_off), 'file_size': hexo(csize),
                             'memory_offset': hexo(seg.vaddr), 'memory_size': hexo(len(seg.data)),
                             'compressed': bool(flags & (1 << index)),
                             'permissions': PERMISSIONS[seg.name], 'sha256': sha256(seg.data)})
        segment_map = {'schema_version': 1, 'kind': 'SegmentMap', 'module': ref,
                       'image_size': hexo(len(image) + nso.bss_size), 'segments': segments,
                       'producer': producer('scripts/inventory-modules.py')}
        mod0 = parse_mod0(image)
        symbols = []
        if mod0:
            mod0_off, fields = mod0
            segment_map['mod0'] = {'offset': hexo(mod0_off), **{k: hexo(v) for k, v in fields.items() if k != 'module_object'}}
            bss_size = fields['bss_end'] - fields['bss_start']
            segments.append({'name': 'bss', 'memory_offset': hexo(fields['bss_start']),
                             'memory_size': hexo(bss_size), 'permissions': 'rw-'})
            symbols = dynamic_symbols(image, fields['dynamic'])

        records = [{'schema_version': 1, 'kind': 'SymbolRecord', 'module': ref, 'offset': hexo(s['value']),
                    'size': s['size'], 'name': s['name'], 'symbol_type': s['type'], 'binding': s['binding'],
                    'defined': s['defined'], 'origin': 'dynsym' if s['defined'] else 'import',
                    'evidence': {'confidence': 'fact', 'source': '.dynsym via MOD0 dynamic section'}}
                   for s in symbols]
        symbol_path = private / 'symbols' / f'{name}.jsonl'
        symbol_path.write_text(''.join(json.dumps(r) + '\n' for r in records))
        symbol_summary[name] = {
            'defined': sum(r['defined'] for r in records),
            'imported': sum(not r['defined'] for r in records),
            'defined_functions': sum(r['defined'] and r['symbol_type'] == 'func' for r in records),
            'output': {'path': str(symbol_path.relative_to(ROOT)), 'sha256': sha256(symbol_path.read_bytes()), 'private': True},
        }

        (public / f'{name}.identity.json').write_text(json.dumps(module_identity, indent=2) + '\n')
        (public / f'{name}.segments.json').write_text(json.dumps(segment_map, indent=2) + '\n')
        print(f'{name}: build_id {nso.build_id[:16]}… image {len(image):#x} '
              f'symbols {symbol_summary[name]["defined"]} defined / {symbol_summary[name]["imported"]} imported')

    symbols_artifact = {
        'schema_version': 1, 'kind': 'AnalysisArtifact', 'analysis': 'dynamic-symbol-extraction',
        'subject': {'title_id': adapter['expected_title_id'], 'modules': refs},
        'producer': producer('scripts/inventory-modules.py'),
        'inputs': [{'role': f'exefs/{n}', 'sha256': sha256((args.exefs / n).read_bytes())} for n in names],
        'outputs': [s['output'] for s in symbol_summary.values()],
        'status': 'passed',
        'results': {n: {k: v for k, v in s.items() if k != 'output'} for n, s in symbol_summary.items()},
        'interpretation': [{'claim': 'Names are those the binary exports or imports dynamically; internal functions stay unnamed.',
                            'evidence': {'confidence': 'fact', 'source': 'ELF dynamic symbol table semantics'}}],
    }
    (public / 'analysis-dynsym.json').write_text(json.dumps(symbols_artifact, indent=2) + '\n')

    if args.decode_tsv:
        row = args.decode_tsv.read_text().splitlines()[0].split('\t')
        status, _, total, unhandled, signatures = row[:5]
        zero, udf, reserved = map(int, row[6:9])
        other = int(unhandled) - zero - udf - reserved
        decode_artifact = {
            'schema_version': 1, 'kind': 'AnalysisArtifact', 'analysis': 'aot-decode-coverage',
            'subject': {'title_id': adapter['expected_title_id'], 'modules': refs},
            'producer': producer('suyu-cmd --probe-decode-list', translate_all=True),
            'inputs': [{'role': 'nsp', 'sha256': identity['sha256']}],
            'status': 'gaps' if status == 'GAPS' else 'passed',
            'results': {'text_words': int(total), 'unhandled_words': int(unhandled),
                        'zero_words': zero, 'udf_words': udf, 'reserved_low_words': reserved,
                        'other_unhandled_words': other, 'top_signatures': signatures.split(' ')},
            'interpretation': [
                {'claim': 'Zero and UDF words are padding/literal data inside .text, not code the emitter must translate.',
                 'evidence': {'confidence': 'heuristic', 'source': 'word classes reported by the probe'}},
                {'claim': f'{other} remaining words (pointer-like/table values) are most likely data; only execution can prove it.',
                 'evidence': {'confidence': 'hypothesis', 'source': 'signature values look like offsets, not encodings',
                              'note': 'Hybrid runs record uncovered code in recomp_gaps.json'}},
            ],
        }
        (public / 'analysis-decode-coverage.json').write_text(json.dumps(decode_artifact, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
