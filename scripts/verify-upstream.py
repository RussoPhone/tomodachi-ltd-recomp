#!/usr/bin/env python3
"""Refuse revision drift before building the external backend."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

def verify(lock_path):
    lock = json.loads(lock_path.read_text())
    repo = ROOT / 'upstream/mk8-recomp'
    actual = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != lock['commit']:
        raise ValueError(f'Backend revision drift: expected {lock["commit"]}, found {actual}')
    rows = subprocess.check_output(['git', '-C', str(repo), 'submodule', 'status', '--recursive'], text=True)
    actual_submodules = {}
    for line in rows.splitlines():
        state = line[0]
        commit, path, *_ = line[1:].split()
        if state != ' ':
            raise ValueError(f'Submodule not at pinned revision: {path}')
        actual_submodules[path] = commit
    expected = {s['path']: s['commit'] for s in lock['submodules']}
    if actual_submodules != expected:
        raise ValueError('Submodule list differs from upstream.lock.json')
    print(f'Verified backend and {len(expected)} pinned submodules.')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--lock', type=Path, default=ROOT / 'upstream.lock.json')
    args = parser.parse_args()
    try:
        verify(args.lock)
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
