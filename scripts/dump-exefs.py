#!/usr/bin/env python3
"""Have suyu decrypt and dump the title's ExeFS/NSOs (its own dump_exefs/dump_nso settings).

Boots suyu-cmd with a private copy of the portable config that enables the dumps, and stops it
as soon as the guest main thread starts (the loader dumps before that). The baseline config is
not modified. Output: local/runtime/user/dump/<TitleID>/{exefs,nso} (private).

  python3 scripts/dump-exefs.py
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import lab  # noqa: E402


def main():
    config = json.loads((ROOT / 'local/runtime.json').read_text())
    title_id = lab.title_id()
    base_ini = ROOT / 'local/runtime/user/config/sdl2-config.ini'
    dump_ini = ROOT / 'local/m2/dump-config.ini'
    dump_ini.parent.mkdir(parents=True, exist_ok=True)
    text = base_ini.read_text() if base_ini.exists() else '[Debugging]\n'
    for key in ('dump_exefs', 'dump_nso'):
        text = re.sub(rf'^{key}\\default=.*$', f'{key}\\\\default=false', text, flags=re.M)
        text, n = re.subn(rf'^{key}=.*$', f'{key}=true', text, flags=re.M)
        if not n:
            text = text.replace('[Debugging]\n', f'[Debugging]\n{key}\\\\default=false\n{key}=true\n', 1)
    dump_ini.write_text(text)

    env = os.environ.copy()
    env['LD_LIBRARY_PATH'] = str(ROOT / 'local/deps/usr/lib')
    log_path = ROOT / 'local/m2/dump-run.log'
    with log_path.open('w') as log:
        proc = subprocess.Popen([str(lab.BIN / 'suyu-cmd'), '-c', str(dump_ini),
                                 '-g', config['dump_path']], cwd=ROOT / 'local/runtime', env=env,
                                stdout=log, stderr=subprocess.STDOUT)
        try:
            for _ in range(120):
                if 'KProcess::Run main thread started' in log_path.read_text(errors='replace') or proc.poll() is not None:
                    break
                time.sleep(1)
            time.sleep(2)
        finally:
            proc.terminate()
            try:
                proc.wait(15)
            except subprocess.TimeoutExpired:
                proc.kill()
    exefs = ROOT / f'local/runtime/user/dump/{title_id}/exefs'
    modules = sorted(p.name for p in exefs.iterdir()) if exefs.is_dir() else []
    print(json.dumps({'exefs': str(exefs.relative_to(ROOT)), 'files': modules}))
    return 0 if 'main' in modules and 'main.npdm' in modules else 1


if __name__ == '__main__':
    sys.exit(main())
