#!/usr/bin/env python3
"""Installer for the unofficial native PC port of Tomodachi Life: Living the Dream (nxrt).

Runs every step in order, shows progress, and resumes where it stopped if interrupted:
  1. check this computer          7. translate the code to C
  2. take your game and keys      8. trim the generated code
  3. download the build tools     9. compile the recompiled game code
  4. apply this project's fixes  10. extract the game data
  5. build the extraction tools  11. build the nxrt runtime (the native port)
  6. extract your game's code    12. create a shortcut (optional)

Nothing from the game ships with this project: everything is generated on your computer from your copy.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import plat  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
STATE = ROOT / 'local/state'
UPSTREAM = ROOT / 'upstream/mk8-recomp'
SUYU = UPSTREAM / 'third_party/suyu'
TARGET = 'tomodachi'
# One no-JIT suyu tree serves dump, export and the final executable (the C++ is compiled once).
os.environ['SWITCHPILER_SUYU_BUILD'] = 'suyu-static'
GREEN, YELLOW, RED, BOLD, RESET = '\033[32m', '\033[33m', '\033[31m', '\033[1m', '\033[0m'
RERUN = 'install.bat' if plat.IS_WINDOWS else './install.sh'
PLAY = 'play.bat' if plat.IS_WINDOWS else './play.sh'
if plat.IS_WINDOWS:
    os.system('')  # turns on ANSI colours in the classic Windows console
    sys.stdout.reconfigure(encoding='utf-8')


def say(text=''):
    print(text, flush=True)


CURRENT_STEP = None  # (index, key, title) of the step being run, for the error report
STEP_LOGS = {}


def scrub(text):
    """Remove personal paths from anything that goes into a report."""
    home = str(Path.home())
    return text.replace(str(ROOT), '<project>').replace(home, '~')


def _cmd(args):
    try:
        out = subprocess.run(args, capture_output=True, text=True, timeout=20)
        return (out.stdout + out.stderr).strip()
    except Exception:  # noqa: BLE001 - a missing tool is just reported as such
        return 'not available'


def write_report(failed=None):
    """Write local/logs/report-<time>.txt: everything needed to investigate an install problem.
    It contains no game data and no key material; personal folders are replaced by ~ and <project>."""
    logs = ROOT / 'local/logs'
    logs.mkdir(parents=True, exist_ok=True)
    path = logs / f"report-{time.strftime('%Y%m%d-%H%M%S')}.txt"
    lines = ['Tomodachi LTD Recomp (nxrt) install report', '=' * 44]
    lines.append('project: ' + _cmd(['git', '-C', str(ROOT), 'log', '-1', '--format=%h %ad %s', '--date=short']))
    lines.append(f'time: {time.strftime("%Y-%m-%d %H:%M:%S")}')
    lines.append(f'failed step: {failed or "none (report requested)"}')
    lines.append('')
    lines.append('--- system ---')
    osr = Path('/etc/os-release')
    if osr.exists():
        lines += [l for l in osr.read_text().splitlines() if l.startswith(('PRETTY_NAME', 'VERSION_ID'))]
    lines.append('kernel: ' + _cmd(['uname', '-sr']))
    cpu = next((l.split(':', 1)[1].strip() for l in Path('/proc/cpuinfo').read_text().splitlines()
                if l.startswith('model name')), 'unknown') if Path('/proc/cpuinfo').exists() else 'unknown'
    lines.append(f'cpu: {cpu} ({os.cpu_count()} threads)')
    try:
        lines.append(f'ram: {ram_gb():.1f} GB, swap: {swap_gb():.1f} GB')
    except Exception:  # noqa: BLE001
        lines.append('ram/swap: unknown')
    lines.append(f'free disk in project: {shutil.disk_usage(ROOT).free / 2**30:.0f} GB')
    lines.append('gpu: ' + _cmd(['nvidia-smi', '--query-gpu=name,driver_version', '--format=csv,noheader']))
    vk = _cmd(['vulkaninfo', '--summary'])
    lines += ['vulkan: ' + l.strip() for l in vk.splitlines() if 'deviceName' in l or 'apiVersion' in l][:6]
    try:
        import doctor
        facts = doctor.check()
        lines.append('')
        lines.append('--- doctor ---')
        for d in facts['vulkan_devices']:
            lines.append(f"vulkan device: {d.get('name')} ({d.get('type')}, api {d.get('api')}, {d.get('driver', '')})")
        lines += [f'PROBLEM: {t}' for t in facts['problems']] + [f'warning: {t}' for t in facts['warnings']]
        lines.append(f"session: {facts['session']}")
    except Exception as error:  # noqa: BLE001 - the report must never fail because of the doctor
        lines.append(f'doctor failed: {error}')
    lines.append('')
    lines.append('--- tools ---')
    for tool, arg in (('python3', '--version'), ('git', '--version'), ('cmake', '--version'), ('ninja', '--version'),
                      ('clang', '--version'), ('clang++', '--version'), ('gcc', '--version')):
        lines.append(f'{tool}: ' + (_cmd([tool, arg]).splitlines() or ['not found'])[0])
    lines.append('')
    lines.append('--- steps ---')
    for i, (key, title, _) in enumerate(STEPS, 1):
        marker, _stamp = stage_paths(i, key)
        lines.append(f'{i:2d}. {"done   " if marker.exists() else "pending"} {title}')
    if failed and CURRENT_STEP:
        log_name = STEP_LOGS.get(CURRENT_STEP[1])
        log = logs / log_name if log_name else None
        if log and log.exists():
            lines += ['', f'--- last 60 lines of {log_name} ---']
            lines += log.read_text(errors='replace').splitlines()[-60:]
    # What the game export left on disk: a module without its runtime header fails much later, in the compile step.
    try:
        for cache in sorted((ROOT / 'local/aot').rglob('aot_cache'))[:2]:
            if not (cache / 'aot_manifest.json').exists():
                continue
            lines += ['', f'--- export folder {cache.relative_to(ROOT)} ---']
            for module in sorted((cache / 'exefs').iterdir())[:12] if (cache / 'exefs').is_dir() else []:
                entries = sorted(e.name for e in module.iterdir())[:14] if module.is_dir() else []
                lines.append(f'{module.name}/ ({len(list(module.iterdir())) if module.is_dir() else 0} files): ' + ', '.join(entries))
        status = ROOT / 'artifacts/aot-export-status.json'
        if status.exists():
            lines += ['', '--- artifacts/aot-export-status.json ---', status.read_text(errors='replace')[:1500]]
    except OSError:
        pass
    # Tools such as cmake/ninja write their own logs under artifacts/; the step log only says "see that file".
    recent = [f for f in sorted((ROOT / 'artifacts').glob('*.log')) if time.time() - f.stat().st_mtime < 6 * 3600] if (ROOT / 'artifacts').is_dir() else []
    for extra in recent[-4:]:
        body = extra.read_text(errors='replace').splitlines()
        # The last lines of a long build are usually warnings; the real error is where ninja prints "FAILED:".
        hits = [i for i, l in enumerate(body) if l.startswith('FAILED:') or ': error:' in l or 'fatal error' in l]
        if hits:
            lines += ['', f'--- first errors in artifacts/{extra.name} ---']
            shown = 0
            for i in hits:
                if shown >= 3:
                    break
                lines += body[max(0, i - 1):i + 14] + ['...']
                shown += 1
        lines += ['', f'--- last 40 lines of artifacts/{extra.name} ---']
        lines += body[-40:]
    path.write_text(scrub('\n'.join(lines)) + '\n')
    return path


def fail(text):
    say(f'\n{RED}{BOLD}✗ {text}{RESET}')
    try:
        report = write_report(CURRENT_STEP[2] if CURRENT_STEP else None)
        say(f'A report to attach to a bug report was saved in {report.relative_to(ROOT)}')
        say(f'(it has no game data or keys; run {RERUN} --report to bundle all logs in one zip)')
    except Exception:  # noqa: BLE001 - never hide the original error
        pass
    say(f'Fix the problem above and run {RERUN} again: it resumes where it stopped.')
    sys.exit(1)


def run(cmd, log, env=None, cwd=ROOT):
    if cmd and cmd[0] == 'git':
        env = plat.git_env(env)
    if CURRENT_STEP:
        STEP_LOGS[CURRENT_STEP[1]] = log
    log = ROOT / 'local/logs' / log
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w') as out:
        rc = subprocess.run([str(c) for c in cmd], cwd=cwd, env=env, stdout=out, stderr=subprocess.STDOUT).returncode
    if rc:
        tail = log.read_text(errors='replace').splitlines()[-15:]
        say('\n'.join('   ' + l for l in tail))
        fail(f'This step failed. The full log is in {log.relative_to(ROOT)}')


def ram_gb():
    return plat.total_ram_mb() / 1024


def swap_gb():
    return plat.swap_mb() / 1024


def atomic_write_text(path, text):
    """Replace a state file only after its complete new contents have been written."""
    path = Path(path)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent,
                                         prefix=f'.{path.name}.', suffix='.tmp', delete=False) as out:
            temporary = Path(out.name)
            out.write(text)
            out.flush()
            os.fsync(out.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def pick(kind, title):
    """Ask for a file or folder with a graphical dialog when available, otherwise in the terminal."""
    if plat.IS_WINDOWS:
        try:
            import tkinter
            from tkinter import filedialog
            root = tkinter.Tk()
            root.withdraw()
            root.attributes('-topmost', True)
            chosen = (filedialog.askdirectory(title=title) if kind == 'dir' else
                      filedialog.askopenfilename(title=title, filetypes=[('Switch game', '*.nsp'), ('All files', '*')]))
            root.destroy()
            if chosen:
                return chosen
        except Exception:  # no Tk: fall back to typing the path
            pass
    if shutil.which('zenity') and (os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY')):
        args = ['zenity', '--file-selection', f'--title={title}'] + (['--directory'] if kind == 'dir' else [])
        res = subprocess.run(args, capture_output=True, text=True)
        if res.returncode == 0 and res.stdout.strip():
            return res.stdout.strip()
    if shutil.which('kdialog') and (os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY')):
        args = ['kdialog', '--title', title]
        if kind == 'dir':
            args += ['--getexistingdirectory', str(Path.home())]
        else:
            args += ['--getopenfilename', str(Path.home()), '*.nsp|Switch game (*.nsp)']
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


def check_windows():
    missing = [t for t in ('git', 'cmake', 'ninja', 'cl', 'clang-cl') if not shutil.which(t)]
    try:
        import cryptography  # noqa: F401
    except ImportError:
        missing.append('the Python package "cryptography"')
    if missing:
        fail('Missing software: ' + ', '.join(missing) + '.\n  Start the installer with install.bat: it installs these.')


@step('Check this computer')
def check():
    if plat.IS_WINDOWS:
        check_windows()
    else:
        check_linux()
    mem, swap = ram_gb(), swap_gb()
    free = shutil.disk_usage(ROOT).free / 2**30
    say(f'   Memory: {mem:.0f} GB (+ {swap:.0f} GB {"page file" if plat.IS_WINDOWS else "swap"}) · Free space: {free:.0f} GB')
    if CI:
        return
    try:  # early look at the GPU: better to learn now than after two hours of compiling
        import doctor
        facts = doctor.check()
        for d in facts['vulkan_devices']:
            say(f"   GPU: {d.get('name')} ({d.get('type')})")
        for text in facts['problems'] + facts['warnings']:
            if 'Vulkan' in text or 'GPU' in text or 'llvmpipe' in text:
                say(f'{YELLOW}   ! {text}{RESET}')
        for text in facts['notes'][:2]:
            say(f'   · {text}')
    except Exception:  # noqa: BLE001 - never block the install because of the GPU check
        pass
    if plat.IS_WINDOWS and mem < 15:
        fail('Compiling the game needs at least 16 GB of RAM.')
    if not plat.IS_WINDOWS and mem + swap < 24:
        fail('Compiling the game needs at least 16 GB of RAM plus 8 GB of swap (24 GB combined).')
    if free < 35:
        fail(f'About 35 GB of free disk space is needed (you have {free:.0f} GB).')


def check_linux():
    if sys.platform != 'linux':
        fail('This installer works on Linux and Windows.')
    missing = [t for t in ('git', 'cmake', 'ninja', 'clang', 'g++', 'glslangValidator') if not shutil.which(t)]
    if not (shutil.which('zenity') or shutil.which('kdialog')):
        say(f'{YELLOW}   Note: install zenity (or kdialog) to type text in the game (names, messages).{RESET}')
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


@step('Download the build tools (mk8-recomp and suyu, open source, GPL)')
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
            content = subprocess.run(['git', '-C', SUYU, 'show', f'HEAD:{f}'], capture_output=True,
                                     env=plat.git_env()).stdout
            (Path(tmp) / f).parent.mkdir(parents=True, exist_ok=True)
            (Path(tmp) / f).write_bytes(content)
        for p in plist:
            res = subprocess.run(['git', 'apply', p], cwd=tmp, capture_output=True, env=plat.git_env())
            if res.returncode:
                # Say which suyu commit this checkout is on, so a mismatch can be told apart from a patch problem.
                head = subprocess.run(['git', '-C', SUYU, 'rev-parse', 'HEAD'], capture_output=True, text=True,
                                      env=plat.git_env()).stdout.strip()
                expected = next(m['commit'] for m in json.loads((ROOT / 'upstream.lock.json').read_text())['submodules']
                                 if m['path'] == 'third_party/suyu')
                log = ROOT / 'local/logs' / f'patch-check-{p.stem}.log'
                log.parent.mkdir(parents=True, exist_ok=True)
                log.write_bytes(res.stderr)
                fail(f'{p.name} does not apply to the pinned suyu version '
                     f'(suyu HEAD {head[:12] or "unknown"}, lock expects {expected[:12]}; git output in {log}).')
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


@step('Build the extraction tools — about 15 to 30 minutes')
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


@step("Translate the game's code to C — a few seconds (a tool window opens and closes by itself; it is not the game)")
def export():
    if not plat.IS_WINDOWS and not (os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY')):
        fail('This step needs your desktop session (run the installer from a terminal window, not over SSH).')
    unit = json.loads((ROOT / f'adapters/{TARGET}/target.json').read_text())['aot']['unit_insns']
    run([sys.executable, 'scripts/export-aot.py', '--backend', 'hybrid', '--unit-insns', str(unit)], 'export-aot.log')


@step('Trim the generated code (makes the compile faster and the program smaller)')
def slim():
    export = current_export()
    run([sys.executable, 'scripts/slim-export.py', export], 'slim-export.log')


@step('Compile the recompiled game — the long part (about 1 to 2 hours)')
def package():
    budget = int((ram_gb() - 3) * 1024)
    say(f'   Using up to {budget // 1024} GB of RAM to compile. You can leave the computer working.')
    run([sys.executable, 'scripts/package-standalone.py', '--jobs', str(jobs()), '--recomp-jobs', '12',
         '--mem-budget-mb', str(budget)], 'package-standalone.log')


@step('Extract the game data (textures, sounds, text) — about 5 minutes')
def install():
    run([sys.executable, 'scripts/install-native.py', '--force'], 'install-native.log')


def current_export():
    """The exefs folder of the export made by the export step with the current settings (older exports may be left over)."""
    aot = json.loads((ROOT / f'adapters/{TARGET}/target.json').read_text())['aot']
    suffix = f"-u{aot['unit_insns']}" if aot.get('unit_insns') else ''
    return find_one('**/aot_cache/exefs', ROOT / 'local/aot' / f"export-{aot['backend']}{suffix}")


def find_one(pattern, base):
    hits = sorted(base.glob(pattern))
    if not hits:
        fail(f'Could not find {pattern} in {scrub(str(base))}. A previous step did not finish: run {RERUN} again.')
    return hits[0]


@step('Build the nxrt runtime (the unofficial native port) — about 20 to 40 minutes')
def nxrt():
    if plat.IS_WINDOWS:
        say('   The native port is not available on Windows yet; skipping (the older build still works).')
        return
    export = current_export()
    recomp = UPSTREAM / 'build/suyu-static/src/suyu_cmd/recomp'
    boost = find_one('.cache/cpm/boost/boost-*', SUYU)
    if not (recomp / 'main').is_dir():
        fail('The compiled game code was not found. Run the installer again so the compile step finishes.')
    say('   This builds the runtime on your computer; it contains code from your game and must not be shared.')
    run(['bash', 'runtime/scripts/install.sh', '--game-export', export, '--recomp-build', recomp,
         '--boost-root', boost, '--prefix', ROOT / 'local/nxrt', '--build-dir', ROOT / 'local/build-nxrt',
         '-j', str(max(2, min(jobs(), 8)))], 'build-nxrt.log')
    if not (ROOT / 'local/nxrt/bin/nxrt').exists():
        fail('The nxrt build finished but the executable is missing. Send the report to the project.')


@step('Create a shortcut (optional)')
def shortcut():
    if plat.IS_WINDOWS or not sys.stdin.isatty():
        return
    answer = input('   Add Tomodachi Life to your applications menu? [Y/n] ').strip().lower()
    if answer not in ('', 'y', 'yes', 's', 'sim'):
        return
    apps = Path.home() / '.local/share/applications'
    apps.mkdir(parents=True, exist_ok=True)
    entry = ('[Desktop Entry]\nType=Application\nName=Tomodachi Life (unofficial native port)\n'
             f'Exec={ROOT / "play.sh"}\nPath={ROOT}\nTerminal=false\nCategories=Game;\n')
    (apps / 'tomodachi-nxrt.desktop').write_text(entry)
    say('   Shortcut added: look for "Tomodachi" in your applications menu.')


CI = '--ci' in sys.argv  # build check without a game: steps 1, 3, 4 and 5 only
CI_STEPS = ('check', 'fetch', 'patches', 'backend')

# Inputs whose changes require rerunning each stage. Later stages depend on earlier ones.
# Changing install.py is conservatively treated as a change to the fetch-and-build pipeline.
# "check" and "choose" retain their resume markers (and never re-prompt for game/keys).
STAGE_INPUTS = {
    'fetch': ('upstream.lock.json', 'scripts/install.py', 'scripts/verify-upstream.py'),
    'patches': ('patches/*.patch',),
    'backend': ('scripts/package-standalone.py', 'scripts/plat.py'),
    'dump': ('scripts/dump-exefs.py', 'scripts/lab.py', f'adapters/{TARGET}/target.json'),
    'export': ('scripts/export-aot.py', f'adapters/{TARGET}/target.json'),
    'slim': ('scripts/slim-export.py',),
    'package': ('scripts/package-standalone.py', 'scripts/compile-meter.py', 'scripts/lab.py', 'scripts/slim-export.py',
                f'adapters/{TARGET}/target.json'),
    'install': ('scripts/install-native.py', 'scripts/inventory-romfs.py', 'scripts/lab.py',
                f'adapters/{TARGET}/target.json'),
    'nxrt': ('runtime/scripts/install.sh', 'runtime/CMakeLists.txt', 'runtime/src/**/*', 'scripts/lab.py'),
}


def stage_fingerprint(key):
    digest = hashlib.sha256()
    for pattern in STAGE_INPUTS[key]:
        digest.update(pattern.encode() + b'\0')
        for path in sorted(ROOT.glob(pattern)):
            if path.is_file():
                digest.update(path.relative_to(ROOT).as_posix().encode() + b'\0' + path.read_bytes())
    return digest.hexdigest()


def stage_paths(index, key):
    stem = STATE / f'{index:02d}-{key}'
    return stem.with_suffix('.ok'), stem.with_suffix('.sha256')


def refresh_after_update():
    """Invalidate changed stages and every downstream stage before resuming.

    Old .ok markers without fingerprints are intentionally considered stale: without a
    previous input snapshot, silently reusing them after a git pull would be unsafe.
    """
    changed_at = None
    for index, (key, _, _) in enumerate(STEPS, 1):
        if key not in STAGE_INPUTS:
            continue
        marker, stamp = stage_paths(index, key)
        if marker.exists() and (not stamp.exists() or stamp.read_text().strip() != stage_fingerprint(key)):
            changed_at = index
            break
        if not marker.exists() and any(stage_paths(j, later)[0].exists()
                                      for j, (later, _, _) in enumerate(STEPS, 1) if j > index):
            changed_at = index
            break
    if changed_at is not None:
        say(f'{YELLOW}Build inputs changed (or old state found): rerunning dependent steps from '
            f'{STEPS[changed_at - 1][0]}.{RESET}')
        for index, (key, _, _) in enumerate(STEPS[changed_at - 1:], changed_at):
            marker, stamp = stage_paths(index, key)
            marker.unlink(missing_ok=True)
            stamp.unlink(missing_ok=True)


def bundle_report():
    import zipfile
    report = write_report()
    out = ROOT / f"install-report-{time.strftime('%Y%m%d-%H%M%S')}.zip"
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        z.write(report, report.name)
        for log in sorted((ROOT / 'local/logs').glob('*.log')):
            z.writestr(log.name, scrub(log.read_text(errors='replace')))
        for log in sorted((ROOT / 'artifacts').glob('*.log')) if (ROOT / 'artifacts').is_dir() else []:
            z.writestr('artifacts-' + log.name, scrub(log.read_text(errors='replace')))
    say(f'Report bundle: {out.name}  (no game data or keys inside; attach it to a GitHub issue)')


def main():
    if '--report' in sys.argv:
        bundle_report()
        return
    if plat.IS_WINDOWS and not CI:
        fail('The native port is Linux-only for now; there is no Windows installer yet.')
    STATE.mkdir(parents=True, exist_ok=True)
    refresh_after_update()
    say(f'{BOLD}Tomodachi Life: Living the Dream — unofficial native PC port (nxrt), installer{RESET}')
    say('Educational, experimental project. Use only with your own copy of the game.\n')
    started = time.time()
    for i, (key, title, fn) in enumerate(STEPS, 1):
        marker, fingerprint_stamp = stage_paths(i, key)
        if CI and key not in CI_STEPS:
            say(f'- {i}/{len(STEPS)} {title} (skipped: needs a game)')
            continue
        if marker.exists():
            say(f'{GREEN}✓{RESET} {i}/{len(STEPS)} {title} (already done)')
            continue
        say(f'{YELLOW}▶{RESET} {BOLD}{i}/{len(STEPS)} {title}{RESET}')
        t0 = time.time()
        global CURRENT_STEP
        CURRENT_STEP = (i, key, title)
        fn()
        if key in STAGE_INPUTS:
            atomic_write_text(fingerprint_stamp, stage_fingerprint(key))
        atomic_write_text(marker, time.strftime('%Y-%m-%d %H:%M:%S'))
        say(f'{GREEN}✓{RESET} done ({(time.time() - t0) / 60:.0f} min)\n')
    say(f'{GREEN}{BOLD}All done!{RESET} ({(time.time() - started) / 60:.0f} min this run)')
    if not CI:
        say(f'To play:  {PLAY}          (fullscreen: {PLAY} --fullscreen)')
        say('The first time you open the face-paint editor it takes about 40 seconds (it prepares a cache on your computer); after that it opens in seconds.')


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        say(f'\nInterrupted. Run {RERUN} again to continue where it stopped.')
        sys.exit(130)
