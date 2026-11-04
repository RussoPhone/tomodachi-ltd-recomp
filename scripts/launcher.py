#!/usr/bin/env python3
"""Launcher for the unofficial native port of Tomodachi Life: Living the Dream (nxrt).

One window with PLAY, the settings that matter (fullscreen, window size, language), save backup/restore and a bug-report button.
The window itself lives in scripts/launcher_ui.py (Island UI Kit style).
Settings are kept in local/launcher.json and handed to scripts/play-nxrt.sh as options. Needs only Python's built-in Tk.
"""
import json
from pathlib import Path
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
LANGUAGES = [
    ('English (US)', 'en-US'), ('English (UK)', 'en-GB'), ('日本語', 'ja'), ('Français', 'fr'),
    ('Français (Canada)', 'fr-CA'), ('Deutsch', 'de'), ('Italiano', 'it'), ('Español', 'es'),
    ('Español (Latinoamérica)', 'es-419'), ('Português (Brasil)', 'pt-BR'), ('Nederlands', 'nl'),
    ('Русский', 'ru'), ('한국어', 'ko'), ('简体中文', 'zh-Hans'), ('繁體中文', 'zh-Hant'),
]
DEFAULTS = {'fullscreen': False, 'size': '1280x720', 'language': 'en-US', 'safe': False}


def load_settings():
    settings = dict(DEFAULTS)
    try:
        settings.update(json.loads(SETTINGS.read_text()))
    except (OSError, ValueError):
        pass
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


def main():
    """Open the window (scripts/launcher_ui.py, drawn in the Island UI Kit style); the logic stays in this file."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import launcher_ui
    launcher_ui.run(sys.modules[__name__], smoke='--smoke' in sys.argv)


if __name__ == '__main__':
    try:
        main()
    except ImportError:
        print('The launcher needs Tk (python3-tk). Start the game with ./play.sh --nogui instead.')
        sys.exit(1)
