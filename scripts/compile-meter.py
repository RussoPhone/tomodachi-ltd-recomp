#!/usr/bin/env python3
"""Compiler launcher: run the command, append '<unit>\t<seconds>\t<peak_rss_kib>\t<rc>' to $COMPILE_METER_LOG.

With COMPILE_OBJ_CACHE=<dir> it is also a content-addressed object cache (ccache is not available
here): the key covers the compiler binary, every flag except output/depfile paths, the source text
and the text of every header in its -I directories, so paths of different exports do not matter.
A hit copies the object and writes a depfile; the log line then ends with '\thit' (or '\tmiss').

With COMPILE_MEM_BUDGET_MB=<n> it is also a memory-aware scheduler: a compile starts only when its
estimated peak (generated C needs ~100 MB of compiler heap per MB of source at any -O level, measured
on this title) fits in the budget next to the compiles already running. Light units then run many at
once and heavy ones alone, so the Ninja pool can be wide without the heavy region exhausting RAM.
"""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import resource
import shutil
import subprocess
import sys
import time

argv = sys.argv[1:]
unit = next((os.path.basename(a) for a in argv if a.endswith('.c')), 'link')
cache_dir = os.environ.get('COMPILE_OBJ_CACHE')


def cache_key(args):
    digest = hashlib.sha256()
    compiler = Path(shutil.which(args[0]) or args[0]).resolve()
    stat = compiler.stat()
    digest.update(f'{compiler}:{stat.st_size}:{stat.st_mtime_ns}\0'.encode())
    skip_next, sources, include_dirs = False, [], []
    for arg in args[1:]:
        if skip_next:
            skip_next = False
            continue
        if arg in ('-o', '-MF', '-MT', '-MQ'):
            skip_next = True
            continue
        if arg.startswith('-I'):
            include_dirs.append(arg[2:].strip('"'))
            digest.update(b'-I\0')
            continue
        if arg.endswith('.c') and not arg.startswith('-'):
            sources.append(arg)
            digest.update(b'<source>\0')
            continue
        digest.update(arg.encode() + b'\0')
    for source in sources:
        digest.update(Path(source).read_bytes())
    for directory in include_dirs:
        for header in sorted(Path(directory).glob('*.h')):
            digest.update(header.name.encode() + b'\0' + header.read_bytes())
    return digest.hexdigest(), sources, include_dirs


def option(args, name):
    return args[args.index(name) + 1] if name in args else None


def estimated_mb(args):
    sources = [a for a in args if a.endswith('.c') and not a.startswith('-')]
    size_mb = sum(os.path.getsize(s) for s in sources if os.path.exists(s)) / 2**20
    return max(300, int(size_mb * 95))  # heavy units peak at ~94 MB per MB of C (clang -O2)


class MemorySlot:
    """Cross-process admission control: {pid: mb} in a JSON file guarded by flock."""

    def __init__(self, budget_mb, want_mb, state_dir):
        self.budget, self.want = budget_mb, min(want_mb, budget_mb)
        state_dir.mkdir(parents=True, exist_ok=True)
        self.lock_path, self.state_path = state_dir / 'mem.lock', state_dir / 'mem.json'

    def _update(self, change):
        with open(self.lock_path, 'a+') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            try:
                state = json.loads(self.state_path.read_text()) if self.state_path.exists() else {}
            except ValueError:
                state = {}
            state = {pid: mb for pid, mb in state.items() if os.path.exists(f'/proc/{pid}')}
            result = change(state)
            self.state_path.write_text(json.dumps(state))
            return result

    def __enter__(self):
        me = str(os.getpid())
        def take(state):
            if not state or sum(state.values()) + self.want <= self.budget:
                state[me] = self.want
                return True
            return False
        while not self._update(take):
            time.sleep(0.5)
        return self

    def __exit__(self, *exc):
        self._update(lambda state: state.pop(str(os.getpid()), None))


start = time.monotonic()
status = ''
key = None
if cache_dir and unit != 'link' and '-c' in argv:
    key, sources, include_dirs = cache_key(argv)
    cached = Path(cache_dir) / f'{key}.o'
    output = option(argv, '-o')
    if cached.exists() and output:
        shutil.copyfile(cached, output)
        depfile = option(argv, '-MF')
        if depfile:
            headers = [str(h) for d in include_dirs for h in sorted(Path(d).glob('*.h'))]
            Path(depfile).write_text(f'{output}: ' + ' '.join(s.replace(' ', '\\ ') for s in sources + headers) + '\n')
        rc, status = 0, '\thit'
    else:
        status = '\tmiss'
if status != '\thit':
    budget = int(os.environ.get('COMPILE_MEM_BUDGET_MB', '0') or 0)
    if budget and unit != 'link' and '-c' in argv:
        state_dir = Path(os.environ.get('COMPILE_MEM_STATE', '/tmp/compile-meter-' + str(os.getuid())))
        with MemorySlot(budget, estimated_mb(argv), state_dir):
            rc = subprocess.call(argv)
    else:
        rc = subprocess.call(argv)
    if key and rc == 0:
        Path(cache_dir).mkdir(parents=True, exist_ok=True)
        tmp = Path(cache_dir) / f'.{key}.{os.getpid()}'
        shutil.copyfile(option(argv, '-o'), tmp)
        os.replace(tmp, Path(cache_dir) / f'{key}.o')
rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
with open(os.environ['COMPILE_METER_LOG'], 'a') as log:
    log.write(f'{unit}\t{time.monotonic() - start:.1f}\t{rss}\t{rc}{status}\n')
sys.exit(rc)
