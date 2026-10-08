#!/usr/bin/env python3
"""Install step: turn the user's own dump into a local, NSP-free game folder.

Writes local/install/<target>/:
  exefs/    main.npdm and the NSOs, decrypted (copied from the suyu ExeFS dump), plus what suyu's
            deconstructed-ROM loader reads beside them: romfs.bin (the decrypted RomFS image, one
            file) and control.nacp / icon_*.dat (title metadata)
  romfs/    (only with --with-files) every RomFS file, decrypted, for modding/analysis
  control/  the Control NCA RomFS (title metadata, icons)
  install.json  what was installed, from which dump (SHA-256), and when

After this the native executable can run from the folder without the NSP or the keys. The folder
holds the user's own game data and translated game code: it is for this machine only and is never
published or distributed (local/ is gitignored).

  python3 scripts/install-native.py [--force]
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import sys
import time

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import lab  # noqa: E402

spec = importlib.util.spec_from_file_location('romfs', ROOT / 'scripts/inventory-romfs.py')
romfs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(romfs)

CHUNK = 8 << 20


def extract_file(reader, offset, size, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    with dest.open('wb') as out:
        done = 0
        while done < size:
            n = min(CHUNK, size - done)
            out.write(reader.read(offset + done, n))
            done += n


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--force', action='store_true')
    parser.add_argument('--with-files', action='store_true', help='also extract every RomFS file (6+ GB more)')
    args = parser.parse_args()
    target = lab.target_name()
    title_id = lab.title_id(target)
    dest = ROOT / f'local/install/{target}'
    marker = dest / 'install.json'
    if marker.exists() and not args.force:
        print(f'already installed at {dest.relative_to(ROOT)} (use --force to redo)')
        return 0
    if dest.exists():
        shutil.rmtree(dest)

    exefs_src = ROOT / f'local/runtime/user/dump/{title_id}/exefs'
    if not (exefs_src / 'main.npdm').exists():
        sys.exit('no ExeFS dump yet: run `switchpiler.py dump` first')
    shutil.copytree(exefs_src, dest / 'exefs')

    key_table = romfs.keys()
    dump = lab.runtime()['dump_path']
    summary = {}
    started = time.time()
    with open(dump, 'rb') as f:
        for name, nca_offset, _ in romfs.pfs0(f):
            if not name.endswith('.nca'):
                continue
            f.seek(nca_offset)
            header = romfs.xts_decrypt(key_table['header_key'], f.read(0xC00))
            content = romfs.CONTENT_TYPES.get(header[0x205], str(header[0x205]))
            if content not in ('program', 'control') or any(header[0x230:0x240]):
                continue
            generation = max(header[0x206], header[0x220])
            generation = generation - 1 if generation else 0
            kak = key_table[f'key_area_key_application_{generation:02x}']
            area = Cipher(algorithms.AES(kak), modes.ECB()).decryptor().update(header[0x300:0x340])
            for section in range(4):
                start, end = struct.unpack_from('<II', header, 0x240 + section * 0x10)
                fs = header[0x400 + section * 0x200:0x600 + section * 0x200]
                if end <= start or fs[3] != 3 or fs[4] != 3 or fs[0x8:0xC] != b'IVFC':
                    continue
                level_offset = struct.unpack_from('<Q', fs, 0x8 + 0x10 + 5 * 0x18)[0]
                reader = romfs.CtrReader(f, nca_offset, area[0x20:0x30], fs[0x140:0x148])
                level_size = struct.unpack_from('<Q', fs, 0x8 + 0x10 + 5 * 0x18 + 8)[0]
                image = dest / 'exefs' / 'romfs.bin' if content == 'program' else None
                if image is not None:  # the whole data level, decrypted in order = a RomFS image
                    extract_file(reader, start * 0x200 + level_offset, level_size, image)
                files = romfs.walk_romfs(reader, start * 0x200 + level_offset)
                root = dest / ('romfs' if content == 'program' else 'control')
                total = 0
                for i, (path, offset, size) in enumerate(files):
                    if content == 'program' and not args.with_files:
                        total += size
                        continue
                    extract_file(reader, offset, size, root / path.lstrip('/'))
                    total += size
                    if content == 'program' and i % 5000 == 0:
                        print(f'  {content}: {i}/{len(files)} files, {total / 2**30:.2f} GiB', flush=True)
                summary[content] = {'files': len(files), 'bytes': total}
                if content == 'program':
                    summary[content]['romfs_bin_bytes'] = level_size
                else:
                    for meta in root.iterdir():
                        if meta.name == 'control.nacp' or meta.name.startswith('icon_'):
                            shutil.copyfile(meta, dest / 'exefs' / meta.name)
    identity = json.loads((ROOT / 'artifacts/input-identity.json').read_text())
    record = {'schema_version': 1, 'title_id': title_id, 'source_nsp_sha256': identity['sha256'],
              'installed_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
              'seconds': round(time.time() - started), 'contents': summary,
              'exefs': sorted(p.name for p in (dest / 'exefs').iterdir()),
              'note': 'local copy of the user\'s own game; never distribute'}
    marker.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record['contents']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
