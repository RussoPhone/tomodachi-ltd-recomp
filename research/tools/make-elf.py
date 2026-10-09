#!/usr/bin/env python3
"""Write a decomp-ready ELF per module: standard AArch64 ELF any RE tool opens (Ghidra, IDA, Binary Ninja, radare2).

Segments come from the NSO (decompressed, module-relative, rebased to --base), and the symbol table carries
everything SWITCHPILER knows, in order of confidence:
  1. dynsym names (fact)                       -> the real name
  2. position-masked signature matches, data-table readers and classes named from their type-name
     strings through their vtables (heuristic) -> the name, recorded as such
  3. every other .eh_frame function (fact: extent known, name unknown) -> sub_<module offset>
Function sizes come from .eh_frame, so tools get exact boundaries instead of guessing.

Outputs local/decomp/<target>/<module>.elf (private: contains the game's code).

  python3 scripts/make-elf.py [--base 0x7100000000]
"""
import argparse
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

PF_X, PF_W, PF_R = 1, 2, 4
SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB, SHT_NOBITS = 1, 2, 3, 8
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4
STT_FUNC, STT_OBJECT, STB_GLOBAL, STB_LOCAL = 2, 1, 1, 0
EM_AARCH64 = 183


def load_jsonl(path):
    return [json.loads(l) for l in path.read_text().splitlines() if l.strip()] if path.exists() else []


def build(module, target, base, out_dir):
    title = lab.title_id(target)
    nso = inventory.Nso(ROOT / f'local/runtime/user/dump/{title}/exefs/{module}')
    private = ROOT / f'local/analysis/{target}'

    names = {}  # offset -> (name, confidence)
    for rec in load_jsonl(private / 'names' / 'data-readers.jsonl'):  # weakest first; stronger sources overwrite
        if rec['module']['name'] == module and int(rec['offset'], 16) not in names:
            names[int(rec['offset'], 16)] = (rec['name'], 'heuristic')
    for rec in load_jsonl(private / 'names' / 'classes.jsonl'):  # scripts/name-classes.py
        if rec['module']['name'] == module:
            names[int(rec['offset'], 16)] = (rec['name'], 'heuristic')
    for rec in load_jsonl(private / 'names' / 'manual.jsonl'):  # names given by reading the code, with evidence
        if rec['module']['name'] == module:
            names[int(rec['offset'], 16)] = (rec['name'], 'heuristic')
    for rec in load_jsonl(private / 'names' / 'signature-matches.jsonl'):
        if rec['module']['name'] == module:
            names[int(rec['offset'], 16)] = (rec['name'], 'heuristic')
    for rec in load_jsonl(private / 'names' / 'sdk-nvn.jsonl'):  # observed: what nvnDeviceGetProcAddress returned
        if rec['module']['name'] == module:
            names[int(rec['offset'], 16)] = (rec['name'], 'fact')
    for rec in load_jsonl(private / 'symbols' / f'{module}.jsonl'):
        if rec['defined'] and rec['symbol_type'] in ('func', 'object'):
            names[int(rec['offset'], 16)] = (rec['name'], 'fact')
    objects = {int(r['offset'], 16): r['size'] for r in load_jsonl(private / 'symbols' / f'{module}.jsonl')
               if r['defined'] and r['symbol_type'] == 'object'}
    for extra in ('classes.jsonl', 'manual.jsonl'):
        objects.update({int(r['offset'], 16): r['size'] or 8 for r in load_jsonl(private / 'names' / extra)
                        if r['module']['name'] == module and r['symbol_type'] == 'object'})
    functions = {int(r['offset'], 16): r['size'] for r in load_jsonl(private / 'functions' / f'{module}.jsonl')}

    segs = [(s, s.vaddr, s.data) for s in nso.segments]
    bss_start = nso.data.end
    bss_size = (struct.unpack_from('<I', open(ROOT / f'local/runtime/user/dump/{title}/exefs/{module}', 'rb').read(0x40), 0x3C)[0])

    # symbol table
    strtab = bytearray(b'\0')
    syms = [struct.pack('<IBBHQQ', 0, 0, 0, 0, 0, 0)]
    text_index, rodata_index, data_index = 1, 2, 3
    def section_for(off):
        if nso.text.vaddr <= off < nso.text.end:
            return text_index
        if nso.rodata.vaddr <= off < nso.rodata.end:
            return rodata_index
        return data_index
    counts = {'fact': 0, 'heuristic': 0, 'unnamed': 0}
    emitted = set()
    for off in sorted(set(functions) | set(names)):
        size = functions.get(off, objects.get(off, 0))
        name, confidence = names.get(off, (f'sub_{off:x}', 'unnamed'))
        if name in emitted:
            name = f'{name}@{off:x}'
        emitted.add(name)
        kind = STT_OBJECT if off in objects and off not in functions else STT_FUNC
        name_off = len(strtab)
        strtab += name.encode('utf-8', 'replace') + b'\0'
        syms.append(struct.pack('<IBBHQQ', name_off, (STB_GLOBAL << 4) | kind, 0, section_for(off), base + off, size))
        counts[confidence] += 1
    symtab = b''.join(syms)

    shstr = bytearray(b'\0')
    def sh_name(n):
        o = len(shstr); shstr.extend(n.encode() + b'\0'); return o
    names_idx = {n: sh_name(n) for n in ('.text', '.rodata', '.data', '.bss', '.symtab', '.strtab', '.shstrtab')}

    # layout: ELF header, program headers, then segment bytes page-aligned, then tables
    phnum, ehsize, phentsize, shentsize = 4, 64, 56, 64
    blob = bytearray(b'\0' * (ehsize + phnum * phentsize))
    def align(n):
        blob.extend(b'\0' * ((-len(blob)) % 0x1000))
    offsets = {}
    for seg, vaddr, data in segs:
        align(0x1000)
        offsets[seg.name] = len(blob)
        blob.extend(data)
    symtab_off = len(blob); blob.extend(symtab)
    strtab_off = len(blob); blob.extend(strtab)
    shstr_off = len(blob); blob.extend(shstr)
    blob.extend(b'\0' * ((-len(blob)) % 8))
    shoff = len(blob)

    flags = {'text': PF_R | PF_X, 'rodata': PF_R, 'data': PF_R | PF_W}
    ph = b''
    for seg, vaddr, data in segs:
        memsz = len(data) + (bss_size if seg.name == 'data' else 0)
        ph += struct.pack('<IIQQQQQQ', 1, flags[seg.name], offsets[seg.name], base + vaddr, base + vaddr,
                          len(data), memsz, 0x1000)
    ph += struct.pack('<IIQQQQQQ', 0x6474e551, PF_R | PF_W, 0, 0, 0, 0, 0, 16)  # PT_GNU_STACK
    blob[ehsize:ehsize + len(ph)] = ph

    sh = [struct.pack('<IIQQQQIIQQ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    for (seg, vaddr, data), sflags in zip(segs, (SHF_ALLOC | SHF_EXECINSTR, SHF_ALLOC, SHF_ALLOC | SHF_WRITE)):
        sh.append(struct.pack('<IIQQQQIIQQ', names_idx['.' + seg.name], SHT_PROGBITS, sflags, base + vaddr,
                              offsets[seg.name], len(data), 0, 0, 0x10, 0))
    sh.append(struct.pack('<IIQQQQIIQQ', names_idx['.bss'], SHT_NOBITS, SHF_ALLOC | SHF_WRITE, base + bss_start,
                          offsets['data'] + len(nso.data.data), bss_size, 0, 0, 0x10, 0))
    sh.append(struct.pack('<IIQQQQIIQQ', names_idx['.symtab'], SHT_SYMTAB, 0, 0, symtab_off, len(symtab), 6, 1, 8, 24))
    sh.append(struct.pack('<IIQQQQIIQQ', names_idx['.strtab'], SHT_STRTAB, 0, 0, strtab_off, len(strtab), 0, 0, 1, 0))
    sh.append(struct.pack('<IIQQQQIIQQ', names_idx['.shstrtab'], SHT_STRTAB, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0))
    blob.extend(b''.join(sh))

    ident = b'\x7fELF' + bytes([2, 1, 1, 0]) + b'\0' * 8
    header = ident + struct.pack('<HHIQQQIHHHHHH', 3, EM_AARCH64, 1, base + nso.text.vaddr, ehsize, shoff, 0,
                                 ehsize, phentsize, phnum, shentsize, len(sh), len(sh) - 1)
    blob[:ehsize] = header
    out = out_dir / f'{module}.elf'
    out.write_bytes(blob)
    return out, counts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--base', default='0x7100000000', help='load address used in the ELF (community default)')
    args = parser.parse_args()
    target = lab.target_name()
    out_dir = ROOT / f'local/decomp/{target}'
    out_dir.mkdir(parents=True, exist_ok=True)
    base = int(args.base, 0)
    for module in ('main', 'sdk', 'rtld'):
        out, counts = build(module, target, base, out_dir)
        print(f'{out.relative_to(ROOT)}: {out.stat().st_size / 2**20:.0f} MiB, symbols {counts}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
