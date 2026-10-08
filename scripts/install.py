#!/usr/bin/env python3
"""Installer for Tomodachi Life: Living the Dream (native PC build).

Runs every step in order, shows progress, and resumes where it stopped if interrupted:
  1. check this computer          6. extract your game's code
  2. take your game and keys      7. translate the code to C
  3. download suyu/mk8-recomp     8. compile the native build
  4. apply this project's fixes    9. extract the game data
  5. build the tools             10. assemble the folder you play from

Nothing from the game ships with this project: everything is generated on your computer from your copy.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
STATE = ROOT / 'local/state'
UPSTREAM = ROOT / 'upstream/mk8-recomp'
SUYU = UPSTREAM / 'third_party/suyu'
TARGET = 'tomodachi'
# One no-JIT suyu tree serves dump, export and the final executable (the C++ is compiled once).
os.environ['SWITCHPILER_SUYU_BUILD'] = 'suyu-static'
GREEN, YELLOW, RED, BOLD, RESET = '\033[32m', '\033[33m', '\033[31m', '\033[1m', '\033[0m'


def say(text=''):
    print(text, flush=True)


def fail(text):
    say(f'\n{RED}{BOLD}✗ {text}{RESET}')
    say('Fix the problem above and run ./install.sh again: it resumes where it stopped.')
    sys.exit(1)


def run(cmd, log, env=None, cwd=ROOT):
    log = ROOT / 'local/logs' / log
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w') as out:
        rc = subprocess.run([str(c) for c in cmd], cwd=cwd, env=env, stdout=out, stderr=subprocess.STDOUT).returncode
    if rc:
        tail = log.read_text(errors='replace').splitlines()[-15:]
        say('\n'.join('   ' + l for l in tail))
        fail(f'This step failed. The full log is in {log.relative_to(ROOT)}')


def ram_gb():
    for line in open('/proc/meminfo'):
        if line.startswith('MemTotal'):
            return int(line.split()[1]) / 2**20
    return 8


def swap_gb():
    for line in open('/proc/meminfo'):
        if line.startswith('SwapTotal'):
            return int(line.split()[1]) / 2**20
    return 0


def pick(kind, title):
    """Ask for a file or folder with a graphical dialog when available, otherwise in the terminal."""
    if shutil.which('zenity') and (os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY')):
        args = ['zenity', '--file-selection', f'--title={title}'] + (['--directory'] if kind == 'dir' else [])
        res = subprocess.run(args, capture_output=True, text=True)
        if res.returncode == 0 and res.stdout.strip():
            return res.stdout.strip()
    return input(f'   {title}\n   Paste the path here and press Enter: ').strip().strip('"\'')


STEPS = []


def step(title):
    def deco(fn):
        STEPS.append((fn.__name__, title, fn))
        return fn
    return deco


@step('Check this computer')
def check():
    missing = [t for t in ('git', 'cmake', 'ninja', 'clang', 'g++', 'glslangValidator') if not shutil.which(t)]
    try:
        import cryptography  # noqa: F401
    except ImportError:
        missing.append('python-cryptography')
    import glob
    cmake_dirs = ['/usr/lib/cmake', '/usr/lib64/cmake', '/usr/lib/x86_64-linux-gnu/cmake', '/usr/local/lib/cmake']
    for package, config in (('qt6-base', 'Qt6Widgets'), ('qt6-svg', 'Qt6Svg'), ('qt6-5compat', 'Qt6Core5Compat'),
                            ('qt6-charts', 'Qt6Charts'), ('quazip-qt6', 'QuaZip-Qt6*'), ('sdl3', 'SDL3')):
        if not any(glob.glob(f'{d}/{config}') for d in cmake_dirs):
            missing.append(package)
    if not any(glob.glob(f'{d}/libavcodec.so*') for d in ('/usr/lib', '/usr/lib64', '/usr/lib/x86_64-linux-gnu')):
        missing.append('ffmpeg')
    if missing:
        fail('Missing software: ' + ', '.join(missing) + '.\n  Install it with the command in "Step 1 — Prepare your computer" in the README.')
    if sys.platform != 'linux':
        fail('For now the installer only works on Linux.')
    mem, swap = ram_gb(), swap_gb()
    free = shutil.disk_usage(ROOT).free / 2**30
    say(f'   Memory: {mem:.0f} GB (+ {swap:.0f} GB swap) · Free space: {free:.0f} GB')
    if mem + swap < 24:
        fail('Compiling the game needs at least 16 GB of RAM plus 8 GB of swap (24 GB combined).')
    if free < 35:
        fail(f'About 35 GB of free disk space is needed (you have {free:.0f} GB).')


@step('Choose your game and your keys')
def choose():
    dump = os.environ.get('TOMODACHI_NSP') or pick('file', 'Choose your Tomodachi Life: Living the Dream file (.nsp)')
    keys = os.environ.get('TOMODACHI_KEYS') or pick('dir', 'Choose the folder that contains your prod.keys')
    dump, keys = Path(dump).expanduser().resolve(), Path(keys).expanduser().resolve()
    if not dump.is_file():
        fail(f'Game file not found: {dump}')
    with open(dump, 'rb') as f:
        if f.read(4) != b'PFS0':
            fail('That file does not look like a Switch .nsp.')
    if not (keys / 'prod.keys').is_file():
        fail(f'No prod.keys found in {keys}')
    user_keys = ROOT / 'local/runtime/user/keys'
    user_keys.mkdir(parents=True, exist_ok=True)
    os.chmod(user_keys, 0o700)
    for name in ('prod.keys', 'title.keys'):
        if (keys / name).is_file():
            shutil.copyfile(keys / name, user_keys / name)
            os.chmod(user_keys / name, 0o600)
    say('   Fingerprinting the file (takes a few seconds)...')
    digest = hashlib.sha256()
    with open(dump, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 24), b''):
            digest.update(chunk)
    (ROOT / 'local/runtime.json').write_text(json.dumps({'dump_path': str(dump), 'target': TARGET}, indent=2))
    (ROOT / 'artifacts').mkdir(exist_ok=True)
    (ROOT / 'artifacts/input-identity.json').write_text(json.dumps(
        {'schema_version': 1, 'sha256': digest.hexdigest(), 'size_bytes': dump.stat().st_size}, indent=2))
    say(f'   Game: {dump.name}\n   Keys: copied into the installer folder (they are never sent anywhere)')


@step('Download suyu/mk8-recomp (open source, GPL)')
def fetch():
    lock = json.loads((ROOT / 'upstream.lock.json').read_text())
    if not (UPSTREAM / '.git').exists():
        UPSTREAM.parent.mkdir(exist_ok=True)
        run(['git', 'clone', lock['repository'], UPSTREAM], 'git-clone.log')
    run(['git', '-C', UPSTREAM, 'checkout', '-q', lock['commit']], 'git-checkout.log')
    say('   Downloading submodules (may take a few minutes)...')
    run(['git', '-C', UPSTREAM, 'submodule', 'update', '--init', '--recursive', '--jobs', '8'], 'git-submodules.log')
    run([sys.executable, 'scripts/verify-upstream.py'], 'verify-upstream.log')


@step("Apply this project's fixes")
def patches():
    import re
    import tempfile
    files = sorted({m for p in sorted((ROOT / 'patches').glob('*.patch'))
                    for m in re.findall(r'^\+\+\+ b/(\S+)', p.read_text(), re.M)})
    plist = sorted((ROOT / 'patches').glob('*.patch'))
    # Build the expected result (pristine files + every patch, in order) in a scratch folder and compare:
    # checking patches one by one cannot tell "applied" apart when two of them touch the same lines.
    with tempfile.TemporaryDirectory() as tmp:
        for f in files:
            content = subprocess.run(['git', '-C', SUYU, 'show', f'HEAD:{f}'], capture_output=True).stdout
            (Path(tmp) / f).parent.mkdir(parents=True, exist_ok=True)
            (Path(tmp) / f).write_bytes(content)
        for p in plist:
            if subprocess.run(['git', 'apply', p], cwd=tmp, capture_output=True).returncode:
                fail(f'{p.name} does not apply to the pinned suyu version.')
        if all((Path(tmp) / f).read_bytes() == (SUYU / f).read_bytes() for f in files):
            say(f'   all {len(plist)} fixes already applied')
            return
    # Not (fully) applied: start these files from the pinned version, then apply every fix in order.
    run(['git', '-C', SUYU, 'checkout', '--', *files], 'patch-reset.log')
    for p in plist:
        run(['git', '-C', SUYU, 'apply', p], f'patch-{p.stem}.log')
        say(f'   applied: {p.name}')


def jobs():
    return max(1, min(os.cpu_count() or 1, int((ram_gb() - 3) / 1.5)))


@step('Build the tools (suyu) — about 15 to 30 minutes')
def backend():
    run([sys.executable, 'scripts/package-standalone.py', '--backend-only', '--jobs', str(jobs())], 'build-backend.log')


@step("Extract your game's code")
def dump():
    run([sys.executable, 'scripts/dump-exefs.py'], 'dump-exefs.log')
    import importlib.util
    spec = importlib.util.spec_from_file_location('nso', UPSTREAM / 'scripts/nso.py')
    nso = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(nso)
    adapter = json.loads((ROOT / f'adapters/{TARGET}/target.json').read_text())
    exefs = ROOT / f"local/runtime/user/dump/{adapter['expected_title_id']}/exefs"
    for module, expected in adapter['expected_modules'].items():
        build_id = nso.Nso(exefs / module).build_id
        if build_id != expected['build_id']:
            fail(f'Your game is a different version than the supported one ({module}). This project supports: '
                 f"{adapter['supported_version']}.")
    say(f"   Version verified: {adapter['supported_version']}")


@step("Translate the game's code to C — a few seconds (a window opens and closes by itself)")
def export():
    if not (os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY')):
        fail('This step needs your desktop session (run the installer from a terminal window, not over SSH).')
    unit = json.loads((ROOT / f'adapters/{TARGET}/target.json').read_text())['aot']['unit_insns']
    run([sys.executable, 'scripts/export-aot.py', '--backend', 'hybrid', '--unit-insns', str(unit)], 'export-aot.log')


@step('Compile the native game — the long part (about 1 to 2 hours)')
def package():
    budget = int((ram_gb() - 3) * 1024)
    say(f'   Using up to {budget // 1024} GB of RAM to compile. You can leave the computer working.')
    run([sys.executable, 'scripts/package-standalone.py', '--jobs', str(jobs()), '--recomp-jobs', '12',
         '--mem-budget-mb', str(budget)], 'package-standalone.log')


@step('Extract the game data (textures, sounds, text) — about 5 minutes')
def install():
    run([sys.executable, 'scripts/install-native.py'], 'install-native.log')


@step('Assemble the folder you play from')
def bundle():
    run([sys.executable, 'scripts/assemble-package.py', '--name', 'tomodachi-native'], 'assemble-package.log')
    desktop = ROOT / 'local/package/tomodachi/tomodachi-native.desktop'
    apps = Path.home() / '.local/share/applications'
    if desktop.exists() and sys.stdin.isatty():
        answer = input('   Add a shortcut to your applications menu? [Y/n] ').strip().lower()
        if answer in ('', 'y', 'yes', 's', 'sim'):
            apps.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(desktop, apps / 'tomodachi-native.desktop')
            say('   Shortcut added: look for "Tomodachi" in your menu.')


def main():
    STATE.mkdir(parents=True, exist_ok=True)
    say(f'{BOLD}Tomodachi Life: Living the Dream — native build installer{RESET}')
    say('Educational, experimental project. Use only with your own copy of the game.\n')
    started = time.time()
    for i, (key, title, fn) in enumerate(STEPS, 1):
        marker = STATE / f'{i:02d}-{key}.ok'
        if marker.exists():
            say(f'{GREEN}✓{RESET} {i}/{len(STEPS)} {title} (already done)')
            continue
        say(f'{YELLOW}▶{RESET} {BOLD}{i}/{len(STEPS)} {title}{RESET}')
        t0 = time.time()
        fn()
        marker.write_text(time.strftime('%Y-%m-%d %H:%M:%S'))
        say(f'{GREEN}✓{RESET} done ({(time.time() - t0) / 60:.0f} min)\n')
    say(f'{GREEN}{BOLD}All done!{RESET} ({(time.time() - started) / 60:.0f} min this run)')
    say('To play:  ./play.sh          (fullscreen: ./play.sh --fullscreen)')


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        say('\nInterrupted. Run ./install.sh again to continue where it stopped.')
        sys.exit(130)
