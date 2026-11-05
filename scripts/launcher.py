#!/usr/bin/env python3
"""Launcher for the unofficial native port of Tomodachi Life: Living the Dream (nxrt).

One window with PLAY, the settings that matter (fullscreen, window size, language), save backup/restore and a bug-report button.
The window itself lives in scripts/launcher_ui.py (Island UI Kit style).
Settings are kept in local/launcher.json and handed to scripts/play-nxrt.sh as options. Needs only Python's built-in Tk.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SETTINGS = ROOT / 'local/launcher.json'
SAVE_DIR = ROOT / 'local/user-nxrt'
BACKUPS = ROOT / 'local/backups'
LOGS = ROOT / 'local/logs'
PLAY = ROOT / 'scripts/play-nxrt.sh'
TESTING_URL = 'https://github.com/RussoPhone/tomodachi-ltd-recomp/blob/main/docs/TESTING.md'
ISSUES_URL = 'https://github.com/RussoPhone/tomodachi-ltd-recomp/issues/new?template=install-or-play-problem.md'

SIZES = ['1280x720', '1600x900', '1920x1080', '2560x1440', '3840x2160']
# The languages the game ships message data for (romfs/Mals: USen EUen JPja EUfr USfr EUde EUit EUes USes EUnl KRko
# CNzh TWzh), as the Switch language codes the runtime hands to the game. Others (e.g. Portuguese, Russian) have no
# text in the game, so choosing them would silently show English.
LANGUAGES = [
    ('English (US)', 'en-US'), ('English (UK)', 'en-GB'), ('日本語', 'ja'), ('Français', 'fr'),
    ('Français (Canada)', 'fr-CA'), ('Deutsch', 'de'), ('Italiano', 'it'), ('Español', 'es'),
    ('Español (Latinoamérica)', 'es-419'), ('Nederlands', 'nl'), ('한국어', 'ko'), ('简体中文', 'zh-Hans'),
    ('繁體中文', 'zh-Hant'),
]
LANGUAGE_CODES = [code for _, code in LANGUAGES]
DEFAULTS = {'fullscreen': False, 'size': '1280x720', 'language': 'en-US', 'safe': False}


LANGUAGE_RESET = None  # the saved language that load_settings() had to replace (for a note in the window)


def load_settings():
    global LANGUAGE_RESET
    settings = dict(DEFAULTS)
    try:
        settings.update(json.loads(SETTINGS.read_text()))
    except (OSError, ValueError):
        pass
    LANGUAGE_RESET = None
    if settings.get('language') not in LANGUAGE_CODES:  # e.g. pt-BR/ru from an older launcher: the game has no such text
        LANGUAGE_RESET = settings.get('language')
        settings['language'] = DEFAULTS['language']
    return settings


def save_settings(settings):
    SETTINGS.parent.mkdir(parents=True, exist_ok=True)
    tmp = SETTINGS.with_suffix('.tmp')
    tmp.write_text(json.dumps(settings, indent=2))
    tmp.replace(SETTINGS)


def play_command(settings):
    cmd = [str(PLAY), '--language', settings['language'], '--size', settings['size']]
    if settings['fullscreen']:
        cmd.append('--fullscreen')
    if settings.get('safe'):
        cmd.append('--safe')
    return cmd


def build_version():
    """Version of the nxrt build that is actually installed ("Alpha 1.7.0"), read from the binary itself.

    The executable prints its version before it rejects the missing --game argument, so this starts nothing
    (about 25 ms). It is NOT taken from CHANGELOG.md: that describes the source, and the installed build can
    be older. Returns '' when there is no build or it does not report a version.
    """
    binary = ROOT / 'local/nxrt/bin/nxrt'
    if not binary.is_file():
        return ''
    try:
        out = subprocess.run([str(binary), '--version'], cwd=ROOT, capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return ''
    match = re.search(r'\bnxrt\s+((?:Alpha|Beta|RC)\s+)?(\d+(?:\.\d+)+[\w.+-]*)', out.stdout + out.stderr)
    return match.group(0).split(None, 1)[1] if match else ''


def installed():
    return (ROOT / 'local/nxrt/bin/nxrt').exists() and (ROOT / 'local/install/tomodachi').is_dir()


KEEP_AUTO = 10


def backup_save(prefix='save'):
    """Zip local/user-nxrt into local/backups/<prefix>-<time>.zip and return the path (None if there is no save yet)."""
    if not SAVE_DIR.is_dir() or not any(SAVE_DIR.iterdir()):
        return None
    BACKUPS.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime('%Y%m%d-%H%M%S')
    out, n = BACKUPS / f'{prefix}-{stamp}.zip', 1
    while out.exists():  # never overwrite an earlier backup (two in the same second)
        n += 1
        out = BACKUPS / f'{prefix}-{stamp}-{n}.zip'
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for path in sorted(SAVE_DIR.rglob('*')):
            if path.is_file():
                z.write(path, path.relative_to(SAVE_DIR).as_posix())
    return out


def auto_backup():
    """Back the save up before each play session and keep only the newest KEEP_AUTO automatic copies (manual ones are never pruned)."""
    try:
        out = backup_save('auto')
        for old in sorted(BACKUPS.glob('auto-*.zip'))[:-KEEP_AUTO]:
            old.unlink()
        return out
    except OSError:
        return None  # a full disk must not stop the game from starting


def restore_save(archive):
    """Replace the save with the contents of `archive`, after backing up the current one. Returns the safety backup path."""
    with zipfile.ZipFile(archive) as z:
        bad = [n for n in z.namelist() if n.startswith('/') or '..' in Path(n).parts]
        if bad:
            raise ValueError('The zip has unsafe paths; refusing to restore it.')
        safety = backup_save()
        if SAVE_DIR.exists():
            shutil.rmtree(SAVE_DIR)
        SAVE_DIR.mkdir(parents=True)
        z.extractall(SAVE_DIR)
    return safety


def open_folder(path):
    path.mkdir(parents=True, exist_ok=True)
    opener = shutil.which('xdg-open')
    if opener:
        subprocess.Popen([opener, str(path)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def make_report():
    """Bundle logs and a system summary (no game data, no keys) with the installer's own report command."""
    result = subprocess.run([sys.executable, str(ROOT / 'scripts/install.py'), '--report'], cwd=ROOT,
                            capture_output=True, text=True)
    zips = sorted(ROOT.glob('install-report-*.zip'))
    return zips[-1] if zips and result.returncode == 0 else None


APP_NAME = 'Tomodachi-LTD-Recomp'
DESKTOP_FILE = 'tomodachi-ltd-recomp.desktop'
OLD_DESKTOP_FILE = 'tomodachi-nxrt.desktop'  # written by earlier installers


def applications_dir():
    return Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local/share') / 'applications'


def _exec_quote(path):
    """Quote a path for the Exec= key of a .desktop file (spaces and the characters the spec escapes)."""
    text = str(path).replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$')
    return f'"{text}"'


def desktop_entry():
    """Menu entry: searching "Tomodachi LTD Recomp" finds it, and it opens the launcher (play.sh falls back to the game)."""
    icon = ROOT / 'assets/icon.svg'
    lines = [
        '[Desktop Entry]', 'Type=Application', 'Version=1.0', f'Name={APP_NAME}',
        'GenericName=Tomodachi Life (unofficial PC port)',
        'Comment=Launcher for the unofficial PC port of Tomodachi Life: Living the Dream. Bring your own copy!',
        'Keywords=Tomodachi;Tomodachi Life;Tomodachi LTD Recomp;LTD;Recomp;Mii;Living the Dream;',
        f'Exec={_exec_quote(ROOT / "play.sh")}', f'Path={ROOT}', 'Terminal=false', 'Categories=Game;',
    ]
    if icon.exists():
        lines.append(f'Icon={icon}')
    return '\n'.join(lines) + '\n'


def install_shortcut(apps=None):
    """Write the menu entry (replacing the old one that pointed at this folder). Returns the file written."""
    apps = Path(apps) if apps else applications_dir()
    apps.mkdir(parents=True, exist_ok=True)
    out = apps / DESKTOP_FILE
    out.write_text(desktop_entry())
    old = apps / OLD_DESKTOP_FILE
    try:
        if old.exists() and str(ROOT) in old.read_text():
            old.unlink()  # same game folder: one entry is enough
    except OSError:
        pass
    updater = shutil.which('update-desktop-database')
    if updater:
        subprocess.run([updater, str(apps)], capture_output=True)
    return out


def remove_shortcut(apps=None):
    apps = Path(apps) if apps else applications_dir()
    removed = []
    for name in (DESKTOP_FILE, OLD_DESKTOP_FILE):
        path = apps / name
        try:
            if path.exists() and (name == DESKTOP_FILE or str(ROOT) in path.read_text()):
                path.unlink()
                removed.append(path)
        except OSError:
            pass
    return removed


def main():
    """Open the window (scripts/launcher_ui.py, drawn in the Island UI Kit style); the logic stays in this file."""
    if '--install-shortcut' in sys.argv:
        print(f'Menu entry written: {install_shortcut()}\nSearch for "Tomodachi LTD Recomp" in your applications menu.')
        return
    if '--remove-shortcut' in sys.argv:
        gone = remove_shortcut()
        print('Removed: ' + ', '.join(map(str, gone)) if gone else 'There was no menu entry to remove.')
        return
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import launcher_ui
    launcher_ui.run(sys.modules[__name__], smoke='--smoke' in sys.argv)


if __name__ == '__main__':
    try:
        main()
    except ImportError:
        print('The launcher needs Tk (python3-tk). Start the game with ./play.sh --nogui instead.')
        sys.exit(1)
