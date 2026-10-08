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

PORT = int(os.environ.get('SUYU_MCP_PORT', '9742'))


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
        while time.time() < deadline:
            if gui.poll() is not None:
                raise RuntimeError(f'suyu exited during export (code {gui.returncode})')
            state = call('get_aot_export_status', timeout=60)
            line = (state.get('progress'), state.get('status'))
            if line != last:
                print(time.strftime('%H:%M:%S'), *line, flush=True)
                last = line
            record['last_status'] = state
            status_path.write_text(json.dumps(record, indent=2) + '\n')
            if state.get('done'):
                break
            time.sleep(15)
        else:
            raise RuntimeError('export timed out')
        record['result'] = 'succeeded' if record['last_status'].get('success') else 'failed'
    except Exception as error:  # record every terminal state, not only success
        record['result'] = 'error'
        record['error'] = str(error)
    finally:
        record['finished'] = time.strftime('%Y-%m-%dT%H:%M:%S%z')
        status_path.write_text(json.dumps(record, indent=2) + '\n')
        if gui.poll() is None:
            gui.terminate()
            try:
                gui.wait(30)
            except subprocess.TimeoutExpired:
                gui.kill()
    print(record['result'], record.get('error', ''))
    return 0 if record['result'] == 'succeeded' else 1


if __name__ == '__main__':
    sys.exit(main())
