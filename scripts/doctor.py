#!/usr/bin/env python3
"""Checks this computer for the native port and says what to fix.

  python3 scripts/doctor.py          human-readable summary (exit code 1 if something blocks the game)
  python3 scripts/doctor.py --json   the same facts as JSON (used by the launcher and the bug report)

It only reads system information (distribution, memory, disk, Vulkan devices); it sends nothing anywhere.
"""
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MIN_RAM_GB = 15       # "16 GB" machines report ~15.x
MIN_RAM_SWAP_GB = 24  # RAM + swap needed for the install
MIN_FREE_GB = 35
MIN_VULKAN = (1, 2)

PACKAGE_HINTS = {
    'vulkaninfo': {'arch': 'vulkan-tools', 'debian': 'vulkan-tools', 'fedora': 'vulkan-tools'},
    'libvulkan': {'arch': 'vulkan-icd-loader + the driver for your GPU (nvidia-utils / vulkan-radeon / vulkan-intel)',
                  'debian': 'libvulkan1 + mesa-vulkan-drivers (AMD/Intel) or the NVIDIA driver',
                  'fedora': 'vulkan-loader + mesa-vulkan-drivers (AMD/Intel) or the NVIDIA driver'},
    'tk': {'arch': 'tk', 'debian': 'python3-tk', 'fedora': 'python3-tkinter'},
}


def run(cmd, timeout=20):
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return (out.stdout + out.stderr).strip()
    except Exception:  # noqa: BLE001 - a missing tool is simply reported as such
        return ''


def distro():
    info = {}
    path = Path('/etc/os-release')
    if path.exists():
        for line in path.read_text().splitlines():
            if '=' in line:
                key, value = line.split('=', 1)
                info[key] = value.strip('"')
    ids = ' '.join([info.get('ID', ''), info.get('ID_LIKE', '')]).lower()
    family = 'arch' if 'arch' in ids else 'debian' if ('debian' in ids or 'ubuntu' in ids) else 'fedora' if (
        'fedora' in ids or 'rhel' in ids) else 'other'
    return {'name': info.get('PRETTY_NAME', platform.system()), 'family': family}


def memory():
    values = {}
    try:
        for line in Path('/proc/meminfo').read_text().splitlines():
            key, rest = line.split(':', 1)
            values[key] = int(rest.split()[0]) / 1048576
    except (OSError, ValueError):
        pass
    return round(values.get('MemTotal', 0), 1), round(values.get('SwapTotal', 0), 1)


def vulkan_devices():
    """Devices listed by `vulkaninfo --summary`: [{'name', 'type', 'api', 'driver'}]."""
    text = run(['vulkaninfo', '--summary'], timeout=30)
    devices, current = [], None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith('GPU') and line.endswith(':'):
            current = {}
            devices.append(current)
        elif current is not None and '=' in line:
            key, value = [p.strip() for p in line.split('=', 1)]
            if key == 'deviceName':
                current['name'] = value
            elif key == 'deviceType':
                current['type'] = value.replace('PHYSICAL_DEVICE_TYPE_', '').lower()
            elif key == 'apiVersion':
                m = re.match(r'(\d+)\.(\d+)', value)
                current['api'] = (int(m.group(1)), int(m.group(2))) if m else None
            elif key in ('driverName', 'driverInfo'):
                current.setdefault('driver', value)
    return [d for d in devices if d.get('name')], bool(text)


def vendor(name):
    low = name.lower()
    if 'nvidia' in low or 'geforce' in low or 'rtx' in low or 'gtx' in low:
        return 'nvidia'
    if 'amd' in low or 'radeon' in low:
        return 'amd'
    if 'intel' in low:
        return 'intel'
    return 'other'


def check():
    dist = distro()
    ram, swap = memory()
    free_gb = round(shutil.disk_usage(ROOT).free / 2**30)
    devices, have_vulkaninfo = vulkan_devices()
    problems, warnings, notes = [], [], []
    hint = lambda key: PACKAGE_HINTS[key].get(dist['family'], PACKAGE_HINTS[key]['arch'])  # noqa: E731

    if platform.system() != 'Linux':
        problems.append('The native port is Linux-only for now.')
    if ram < MIN_RAM_GB:
        problems.append(f'Only {ram} GB of RAM; 16 GB is recommended.')
    if ram + swap < MIN_RAM_SWAP_GB:
        warnings.append(f'RAM + swap is {ram + swap:.0f} GB; the install compiles big files and wants about {MIN_RAM_SWAP_GB} GB '
                        '(add swap if the install runs out of memory).')
    if free_gb < MIN_FREE_GB:
        warnings.append(f'{free_gb} GB free here; the install needs about {MIN_FREE_GB} GB.')

    if not have_vulkaninfo:
        warnings.append(f'`vulkaninfo` not found, so the GPU could not be checked. Install: {hint("vulkaninfo")}.')
    elif not devices:
        problems.append('No Vulkan device found. Install the Vulkan loader and your GPU driver: ' + hint('libvulkan') + '.')
    else:
        real = [d for d in devices if d.get('type') not in ('cpu', None) and 'llvmpipe' not in d['name'].lower()]
        if not real:
            problems.append('Only a software Vulkan device (llvmpipe) was found: the game would be far too slow. '
                            'Install the Vulkan driver for your GPU: ' + hint('libvulkan') + '.')
        for d in real:
            api = d.get('api')
            if api and api < MIN_VULKAN:
                problems.append(f'{d["name"]} reports Vulkan {api[0]}.{api[1]}; 1.2 or newer is needed. Update the driver.')
            v = vendor(d['name'])
            if v in ('amd', 'intel'):
                notes.append(f'{d["name"]}: AMD and Intel GPUs are untested with the native port. If the picture looks wrong, '
                             'try ./play.sh --nogui --safe and tell us what happened (docs/COMPATIBILITY.md).')
            elif v == 'nvidia':
                notes.append(f'{d["name"]}: NVIDIA with the proprietary driver is the tested setup (RTX 4050).')
            elif v == 'other':
                notes.append(f'{d["name"]}: untested GPU.')
        if len(real) > 1:
            notes.append('More than one GPU found; the port picks a discrete GPU first. Force one with VK_DRIVER_FILES or DRI_PRIME.')

    session = os.environ.get('XDG_SESSION_TYPE', '')
    if not (os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY')):
        warnings.append('No graphical session detected (DISPLAY/WAYLAND_DISPLAY empty): the game needs a desktop.')
    elif session == 'wayland' and not os.environ.get('DISPLAY'):
        warnings.append('Wayland without XWayland: the game starts through X11/XWayland; enable XWayland.')
    try:
        import tkinter  # noqa: F401
    except ImportError:
        warnings.append(f'Tk is missing, so the launcher window will not open (the game still starts with ./play.sh --nogui). '
                        f'Install: {hint("tk")}.')
    for tool in ('git', 'cmake', 'ninja', 'clang', 'python3'):
        if not shutil.which(tool):
            warnings.append(f'`{tool}` not found (needed by the installer).')

    return {'distribution': dist['name'], 'kernel': platform.release(), 'session': session or 'unknown',
            'ram_gb': ram, 'swap_gb': swap, 'free_disk_gb': free_gb,
            'vulkan_devices': [{k: (list(v) if isinstance(v, tuple) else v) for k, v in d.items()} for d in devices],
            'problems': problems, 'warnings': warnings, 'notes': notes}


def main():
    facts = check()
    if '--json' in sys.argv:
        print(json.dumps(facts, indent=2))
        return 1 if facts['problems'] else 0
    print(f"System:  {facts['distribution']} (kernel {facts['kernel']}, session {facts['session']})")
    print(f"Memory:  {facts['ram_gb']} GB RAM + {facts['swap_gb']} GB swap; {facts['free_disk_gb']} GB free in this folder")
    for d in facts['vulkan_devices']:
        api = d.get('api')
        print(f"GPU:     {d['name']} ({d.get('type', '?')}, Vulkan {api[0]}.{api[1] if api else '?'}"
              f"{', ' + d['driver'] if d.get('driver') else ''})" if api else f"GPU:     {d['name']}")
    for label, key in (('PROBLEM', 'problems'), ('Warning', 'warnings'), ('Note', 'notes')):
        for text in facts[key]:
            print(f'{label}: {text}')
    if not facts['problems']:
        print('\nNothing blocks the game on this computer.' + (' (See the warnings above.)' if facts['warnings'] else ''))
    return 1 if facts['problems'] else 0


if __name__ == '__main__':
    sys.exit(main())
