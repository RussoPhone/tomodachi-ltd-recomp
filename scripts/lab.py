"""Shared paths and target resolution for the lab scripts.

The active target is local/runtime.json "target" (default "tomodachi"); per-game values live in
adapters/<target>/target.json so the scripts themselves stay game-agnostic.
"""
import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / 'upstream/mk8-recomp/build/suyu/bin'


def runtime():
    return json.loads((ROOT / 'local/runtime.json').read_text())


def target_name():
    return os.environ.get('SWITCHPILER_TARGET') or runtime().get('target', 'tomodachi')


def adapter(name=None):
    return json.loads((ROOT / f'adapters/{name or target_name()}/target.json').read_text())


def title_id(name=None):
    return adapter(name)['expected_title_id'].upper()


def aot_settings(name=None):
    """Adapter AOT knobs with lab defaults."""
    settings = {'backend': 'hybrid', 'unit_insns': 0, 'opt_flags': '', 'jobs': 1}
    settings.update(adapter(name).get('aot', {}))
    return settings


def export_name(name=None):
    settings = aot_settings(name)
    suffix = f'-u{settings["unit_insns"]}' if settings['unit_insns'] else ''
    return f'export-{settings["backend"]}{suffix}'


def suyu_env(base=None, isolate_config=True):
    """Environment for launching suyu: no inherited AOT/recomp switches, local Qt Charts, isolated QSettings."""
    env = dict(base if base is not None else os.environ)
    for key in list(env):
        if key.startswith(('SUYU_RECOMP_', 'SUYU_AOT_')):
            del env[key]
    env['LD_LIBRARY_PATH'] = str(ROOT / 'local/deps/usr/lib') + (
        ':' + env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
    if isolate_config:
        env['XDG_CONFIG_HOME'] = str(ROOT / 'local/xdg-config')
    return env
