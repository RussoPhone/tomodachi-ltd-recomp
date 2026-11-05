#!/usr/bin/env python3
"""Drive suyu's AOT exporter (Qt GUI, over its MCP server) for the lab's target.

Mirrors upstream scripts/export-static-title.ps1 for Linux. The GUI runs from the
portable profile (cwd local/runtime) with QSettings redirected into local/, so
neither the desktop suyu profile nor ~/.config is touched.

  python3 scripts/export-aot.py [--backend hybrid|static] [--format source|build]
      [--unit-insns N]   # lab patch 0002: cap guest instructions per generated unit
"""
import argparse
import json
import os
import re
from pathlib import Path
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import lab  # noqa: E402
sys.path.insert(0, str(ROOT / 'upstream/mk8-recomp/scripts'))
from mcp import rpc  # noqa: E402

OPEN_FILES_NEEDED = 4096  # the export opens ~1700 files at once, plus what Qt and the libraries hold
PORT = int(os.environ.get('SUYU_MCP_PORT', '9742'))


def raise_open_file_limit(wanted=65536):
    """The exporter keeps every generated .c file of a module open at once (1650 for this game) and fails silently once the process runs
    out of file descriptors. Linux gives a process 1024 by default, so the export stopped at about 970 files (issue #17). Raise the soft
    limit up to the hard one; the suyu we start inherits it. Returns the limit in force, or None where there is no such limit (Windows)."""
    try:
        import resource
    except ImportError:
        return None
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    if hard == resource.RLIM_INFINITY:
        target = wanted
    else:
        target = min(wanted, hard)
    if soft == resource.RLIM_INFINITY or soft >= target:
        return soft
    try:
        resource.setrlimit(resource.RLIMIT_NOFILE, (target, hard))
    except (OSError, ValueError):
        return soft
    return target


def port_open():
    try:
        with socket.create_connection(('127.0.0.1', PORT), timeout=1):
            return True
    except OSError:
        return False


def call(name, args=None, timeout=30.0):
    out = rpc('tools/call', {'name': name, 'arguments': args or {}}, timeout=timeout)
    for block in out.get('result', {}).get('content', []):
        if block.get('type') == 'text':
            try:
                return json.loads(block['text'])
            except json.JSONDecodeError:
                return {'_text': block['text']}
    return out


def tree_state(root):
    """(file count, total bytes) of everything under `root`; used to see when the export has stopped writing."""
    count = size = 0
    for path in Path(root).rglob('*'):
        try:
            if path.is_file():
                count += 1
                size += path.stat().st_size
        except OSError:
            pass
    return count, size


def wait_until_settled(root, quiet_s=10, max_s=300, poll_s=2):
    """The status says "done" while the biggest module is still being written (a user's `main` came out without src/recompiled_main_972.c
    and recomp_runtime.h, issue #17). Wait until the output has not changed for `quiet_s` seconds. Returns the seconds waited."""
    start = last_change = time.time()
    state = tree_state(root)
    while time.time() - start < max_s:
        time.sleep(poll_s)
        now = tree_state(root)
        if now != state:
            state, last_change = now, time.time()
        elif time.time() - last_change >= quiet_s:
            break
    return time.time() - start


def missing_sources(root):
    """Files that a module's CMakeLists.txt lists as sources but that are not on disk (an export that stopped half way)."""
    missing = []
    for cmake in Path(root).rglob('CMakeLists.txt'):
        if cmake.parent.name == 'aot_cache' or not (cmake.parent / 'recomp_export.c').exists():
            continue
        for name in sorted(set(re.findall(r'\bsrc/[A-Za-z0-9_]+\.c\b', cmake.read_text(errors='replace')))):
            if not (cmake.parent / name).is_file():
                missing.append(f'{cmake.parent.name}/{name}')
    return missing


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--backend', default='hybrid', choices=['hybrid', 'static'])
    parser.add_argument('--format', default='source', choices=['source', 'build'])
    parser.add_argument('--timeout-min', type=int, default=240)
    parser.add_argument('--unit-insns', type=int, default=0)
    args = parser.parse_args()

    config = json.loads((ROOT / 'local/runtime.json').read_text())
    rom = config['dump_path']
    suffix = f'-u{args.unit_insns}' if args.unit_insns else ''
    out_dir = ROOT / f'local/aot/export-{args.backend}{suffix}'
    status_path = ROOT / 'artifacts/aot-export-status.json'
    gui_log = ROOT / 'artifacts/aot-export-gui.log'
    if port_open():
        sys.exit(f'MCP port {PORT} already in use; refusing to interfere with another suyu')
    status_path.parent.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    for name in list(env):
        if name.startswith(('SUYU_RECOMP_', 'SUYU_AOT_')):
            del env[name]
    env['SUYU_AOT_TRANSLATE_ALL'] = '1'
    if args.unit_insns:
        env['SUYU_AOT_UNIT_INSNS'] = str(args.unit_insns)
    env['SUYU_MCP_PORT'] = str(PORT)
    env['XDG_CONFIG_HOME'] = str(ROOT / 'local/xdg-config')
    env['LD_LIBRARY_PATH'] = str(ROOT / 'local/deps/usr/lib') + (
        ':' + env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
    exe = lab.tool('suyu')
    lab.portable_profile()
    limit = raise_open_file_limit()
    if limit is not None and limit < OPEN_FILES_NEEDED:
        sys.exit(f'This session allows only {limit} open files and cannot raise it; the export needs {OPEN_FILES_NEEDED}. '
                 f'Run "ulimit -n 65536" in this terminal (or raise the limit in /etc/security/limits.conf) and run the installer again.')
    with gui_log.open('w') as log:
        gui = subprocess.Popen([str(exe), '-hacker'], cwd=ROOT / 'local/runtime', env=env,
                               stdout=log, stderr=subprocess.STDOUT)
    record = {'schema_version': 1, 'backend': args.backend, 'format': args.format, 'unit_insns': args.unit_insns,
              'output_dir': str(out_dir.relative_to(ROOT)), 'started': time.strftime('%Y-%m-%dT%H:%M:%S%z')}
    try:
        for _ in range(120):
            if port_open() or gui.poll() is not None:
                break
            time.sleep(1)
        if not port_open():
            raise RuntimeError(f'MCP did not come up (gui exit {gui.poll()})')
        try:  # export_game blocks its handler inside dialog.exec(); a timeout is expected
            rpc('tools/call', {'name': 'trigger_ui_action', 'arguments': {'action': 'export_game'}}, timeout=3)
        except (TimeoutError, socket.timeout):
            pass
        time.sleep(3)
        reply = call('trigger_ui_action', {'action': 'aot_test_export', 'rom_path': rom,
                                           'output_dir': str(out_dir), 'format': args.format,
                                           'backend': args.backend, 'full_scan': True}, timeout=60)
        record['trigger'] = reply
        deadline = time.time() + args.timeout_min * 60
        last = None
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                raise RuntimeError('export timed out')
            if gui.poll() is not None:
                raise RuntimeError(f'suyu exited during export (code {gui.returncode})')
            state = call('get_aot_export_status', timeout=min(60, remaining))
            line = (state.get('progress'), state.get('status'))
            if line != last:
                print(time.strftime('%H:%M:%S'), *line, flush=True)
                last = line
            record['last_status'] = state
            status_path.write_text(json.dumps(record, indent=2) + '\n')
            if state.get('done'):
                break
            time.sleep(min(15, remaining))
        record['result'] = 'succeeded' if record['last_status'].get('success') else 'failed'
        if record['result'] == 'succeeded':
            waited = wait_until_settled(out_dir)
            print(f'{time.strftime("%H:%M:%S")} export written; waited {waited:.0f} s for the files to settle', flush=True)
            lacking = missing_sources(out_dir)
            if lacking:
                record['result'] = 'failed'
                record['error'] = f'{len(lacking)} source file(s) listed by the export are missing, for example {", ".join(lacking[:3])}'
    except Exception as error:  # record every terminal state, not only success
        record['result'] = 'error'
        record['error'] = str(error)
    finally:
        record['finished'] = time.strftime('%Y-%m-%dT%H:%M:%S%z')
        if gui.poll() is None:
            gui.terminate()
            try:
                gui.wait(30)
            except subprocess.TimeoutExpired:
                gui.kill()
                gui.wait()
        try:
            status_path.write_text(json.dumps(record, indent=2) + '\n')
        except OSError as error:
            record['result'] = 'error'
            message = f'could not write export status {status_path}: {error}'
            record['error'] = f"{record['error']}; {message}" if record.get('error') else message
            print(message, file=sys.stderr)
    print(record['result'], record.get('error', ''))
    return 0 if record['result'] == 'succeeded' else 1


if __name__ == '__main__':
    sys.exit(main())
