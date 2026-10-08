#!/usr/bin/env python3
"""Assemble the final, self-contained game folder from the standalone executable and the local install.

local/package/<target>/
  <name>[.exe]       the standalone executable (hardlink: no second copy of 2+ GB), plus its DLLs on Windows
  game/              exefs + romfs.bin + control metadata from local/install/<target> (hardlinks)
  user/              the game's own profile: config, saves, shader cache, logs; keys are never needed
  run.sh / run.bat   launcher with relative paths (the folder can be moved as a whole);
                     options: --scale 1|1.5|2|3|4 (internal resolution), --fullscreen
  <name>.desktop     Linux menu entry template (not installed automatically)
  icon.jpg           the title's own icon, from its control data
  README.txt

The folder holds this user's own game data and translated game code. It is for this machine only and is
never published or distributed; what is shared is SWITCHPILER itself.

  python3 scripts/assemble-package.py [--name tomodachi-native]
"""
import argparse
import os
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import lab  # noqa: E402
import plat  # noqa: E402


def link_or_copy(src, dst):
    try:
        os.link(src, dst)
    except OSError:  # another drive, or a file system without hard links
        shutil.copyfile(src, dst)


def hardlink_tree(src, dst):
    for path in src.rglob('*'):
        target = dst / path.relative_to(src)
        if path.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists():
                target.unlink()
            link_or_copy(path, target)


SCALES = {'0.5': 1, '1': 3, '1.5': 5, '2': 6, '3': 7, '4': 8}  # Settings::ResolutionSetup values


def set_ini(path, section, values):
    """Set keys (and their \\default=false twins) inside one [section] of a suyu ini, adding what is missing."""
    lines = path.read_text().splitlines() if path.exists() else []
    if f'[{section}]' not in lines:
        lines += ['', f'[{section}]']
    start = lines.index(f'[{section}]') + 1
    end = next((i for i in range(start, len(lines)) if lines[i].startswith('[')), len(lines))
    body = [l for l in lines[start:end] if l.split('=')[0].split('\\')[0] not in values]
    for key, value in values.items():
        body += [f'{key}\\default=false', f'{key}={value}']
    path.write_text('\n'.join(lines[:start] + body + lines[end:]) + '\n')


def seed_shader_cache(pkg, title):
    """Keep the package's shader cache at least as complete as any compatible cache made on this machine."""
    title = title.lower()
    dest = pkg / 'user/cache/shader' / title
    best, best_size = None, 0
    for cand in ROOT.glob(f'local/**/cache/shader/{title}/vulkan_pipelines.bin'):
        if cand.is_relative_to(pkg):
            continue
        if cand.stat().st_size > best_size:
            best, best_size = cand.parent, cand.stat().st_size
    current = dest / 'vulkan_pipelines.bin'
    if best and best_size > (current.stat().st_size if current.exists() else 0):
        if dest.exists():
            backup = dest.with_name(dest.name + '.previous')
            shutil.rmtree(backup, ignore_errors=True)
            shutil.copytree(dest, backup)
        dest.mkdir(parents=True, exist_ok=True)
        for f in best.iterdir():
            shutil.copyfile(f, dest / f.name)
        return f'seeded from {best.relative_to(ROOT)} ({best_size // 1024} KiB of pipelines)'
    return 'kept (already the largest)'


def write_windows_launcher(pkg, name):
    scales = '; '.join(f"'{k}' = {v}" for k, v in SCALES.items())
    (pkg / 'run.ps1').write_text(f'''# Start the native build from this folder; the folder can be moved as a whole.
#   run.bat [--scale 0.5|1|1.5|2|3|4] [--fullscreen]
Set-Location -LiteralPath $PSScriptRoot
$ini = 'user\\config\\sdl2-config.ini'
$scales = @{{ {scales} }}
$argv = $args  # switch blocks have their own $args
$extra = @()
for ($i = 0; $i -lt $argv.Count; $i++) {{
  switch ($argv[$i]) {{
    '--scale' {{
      $value = $scales[[string]$argv[$i + 1]]
      if ($null -eq $value) {{ Write-Host "unknown scale $($argv[$i + 1]) (use 0.5 1 1.5 2 3 4)"; exit 2 }}
      if (Test-Path $ini) {{
        $text = [IO.File]::ReadAllText($ini)
        $text = $text -replace '(?m)^resolution_setup\\\\default=[^\\r\\n]*', 'resolution_setup\\default=false'
        $text = $text -replace '(?m)^resolution_setup=[^\\r\\n]*', "resolution_setup=$value"
        [IO.File]::WriteAllText($ini, $text)
      }}
      $i++
    }}
    '--fullscreen' {{ $extra += '-f' }}
    default {{ $extra += $argv[$i] }}
  }}
}}
& ".\\{name}.exe" @extra -g 'game\\main'
exit $LASTEXITCODE
''')
    (pkg / 'run.bat').write_text(
        '@echo off\r\nrem Start the game. Options: --fullscreen   --scale 2 (sharper image; 1, 1.5, 2, 3 or 4)\r\n'
        'powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run.ps1" %*\r\n', newline='')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--name', default=None)
    args = parser.parse_args()
    target = lab.target_name()
    name = args.name or f'{target}-native'
    exe = ROOT / f'upstream/mk8-recomp/build/suyu-static/bin/suyu-cmd-static{plat.EXE}'
    install = ROOT / f'local/install/{target}/exefs'
    if not exe.exists():
        sys.exit('no standalone executable: run `switchpiler.py package` first')
    if not (install / 'romfs.bin').exists():
        sys.exit('no local install: run `switchpiler.py install` first')
    pkg = ROOT / f'local/package/{target}'
    pkg.mkdir(parents=True, exist_ok=True)

    binary = pkg / (name + plat.EXE)
    if binary.exists():
        binary.unlink()
    link_or_copy(exe, binary)
    for dll in exe.parent.glob('*.dll'):  # Windows: SDL3, FFmpeg, OpenSSL, ... beside the .exe
        if (pkg / dll.name).exists():
            (pkg / dll.name).unlink()
        link_or_copy(dll, pkg / dll.name)
    if (pkg / 'game').exists():
        shutil.rmtree(pkg / 'game')
    hardlink_tree(install, pkg / 'game')
    (pkg / 'user/config').mkdir(parents=True, exist_ok=True)
    # The export reads keys/firmware from the "installed suyu" this file names; naming the
    # executable itself makes that the package's own user/ (and a self-contained install needs neither).
    (pkg / 'user/config/suyu-install.txt').write_text(f'../../{name}{plat.EXE}\n')

    print('shader cache:', seed_shader_cache(pkg, lab.title_id(target)))
    set_ini(pkg / 'user/config/sdl2-config.ini', 'Renderer',
            {'use_asynchronous_shaders': 'true', 'use_disk_shader_cache': 'true'})

    icon = install / 'icon_AmericanEnglish.dat'
    if icon.exists():
        shutil.copyfile(icon, pkg / 'icon.jpg')
    if plat.IS_WINDOWS:
        write_windows_launcher(pkg, name)
    run = pkg / 'run.sh'
    scale_cases = '\n'.join(f'      {k}) set_res {v} ;;' for k, v in SCALES.items())
    run.write_text(f'''#!/bin/sh
# Start the native build from this folder; the folder can be moved as a whole.
#   ./run.sh [--scale 0.5|1|1.5|2|3|4] [--fullscreen]
cd "$(dirname "$(readlink -f "$0")")" || exit 1
INI=user/config/sdl2-config.ini
set_res() {{
  [ -f "$INI" ] || return
  sed -i -e 's/^resolution_setup\\\\default=.*/resolution_setup\\\\default=false/' \\
         -e "s/^resolution_setup=.*/resolution_setup=$1/" "$INI"
}}
EXTRA=""
while [ $# -gt 0 ]; do
  case "$1" in
    --scale)
      case "$2" in
{scale_cases}
        *) echo "unknown scale $2 (use 0.5 1 1.5 2 3 4)"; exit 2 ;;
      esac
      shift 2 ;;
    --fullscreen) EXTRA="$EXTRA -f"; shift ;;
    *) EXTRA="$EXTRA $1"; shift ;;
  esac
done
exec ./{name} $EXTRA -g game/main
''')
    run.chmod(0o755)
    title = lab.adapter(target).get('name', target)
    (pkg / f'{name}.desktop').write_text(
        '[Desktop Entry]\nType=Application\n'
        f'Name={title} (native)\nComment=Native build made locally by SWITCHPILER from your own copy\n'
        f'Exec={pkg / "run.sh"}\nIcon={pkg / "icon.jpg"}\nTerminal=false\nCategories=Game;\n')
    (pkg / 'README.txt').write_text(
        f'{title} - native build (made by SWITCHPILER on this machine)\n\n'
        'Start: ./run.sh   (or copy the .desktop file into ~/.local/share/applications)\n\n'
        'This folder contains your own game data and machine-translated game code.\n'
        'It is for your personal use on this machine. Do not share or upload it.\n'
        'The translated C is generated from the binary; it is not the original source code.\n')
    size = sum(p.stat().st_size for p in pkg.rglob('*') if p.is_file())
    print(f'package ready: {pkg.relative_to(ROOT)} ({size / 2**30:.1f} GiB apparent, hardlinked)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
