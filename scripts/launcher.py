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
import urllib.request
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
DEFAULTS = {'check_updates': True, 'fullscreen': False, 'size': '1280x720', 'language': 'en-US', 'safe': False}


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


def play_command(settings, user_dir=None):
    """user_dir: the save folder to play with. The default one (local/user-nxrt) is not passed, so it also works
    with an older scripts/play-nxrt.sh that has no --user."""
    cmd = [str(PLAY), '--language', settings['language'], '--size', settings['size']]
    if user_dir is not None and Path(user_dir) != SAVE_DIR:
        cmd += ['--user', str(user_dir)]
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


RELEASES_API = 'https://api.github.com/repos/RussoPhone/tomodachi-ltd-recomp/releases?per_page=10'
UPDATE_CACHE = ROOT / 'local/update-check.json'
UPDATE_EVERY = 6 * 3600  # seconds between looks at GitHub; the result is cached, so a start never waits for the network


def version_key(text):
    """'Alpha 1.7.4', 'v1.7.4' or '1.7.4-rc1' -> (1, 7, 4); () when there is no number."""
    match = re.search(r'(\d+(?:\.\d+)+)', text or '')
    return tuple(int(n) for n in match.group(1).split('.')) if match else ()


def fetch_latest_release(timeout=4):
    """Newest published release of the project, or None (offline, rate-limited, anything unexpected).

    One read-only GET to the public GitHub API; nothing about this computer is sent. Every release is a
    pre-release, so /releases/latest would not do: the list is read and the highest version wins.
    """
    request = urllib.request.Request(RELEASES_API, headers={
        'Accept': 'application/vnd.github+json', 'User-Agent': 'tomodachi-ltd-recomp-launcher'})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            releases = json.load(response)
    except (OSError, ValueError):
        return None
    best = None
    for release in releases if isinstance(releases, list) else []:
        if not isinstance(release, dict) or release.get('draft'):
            continue
        key = version_key(str(release.get('tag_name', '')))
        if key and (best is None or key > best[0]):
            best = (key, release)
    if best is None:
        return None
    release = best[1]
    title = str(release.get('name') or '')
    version = re.match(r'(?:Alpha|Beta|RC)\s+\d+(?:\.\d+)+', title)
    return {'tag': str(release['tag_name']), 'url': str(release.get('html_url') or ''),
            'version': version.group(0) if version else str(release['tag_name']).lstrip('v'),
            'title': title.split(':', 1)[1].strip() if ':' in title else ''}


def cached_release():
    """(release or None, fresh) from the last check; fresh means younger than UPDATE_EVERY."""
    try:
        data = json.loads(UPDATE_CACHE.read_text())
        return data['release'], time.time() - float(data['checked']) < UPDATE_EVERY
    except (OSError, ValueError, KeyError, TypeError):
        return None, False


def check_for_update(force=False):
    """Latest release for the window: the cache when it is fresh, else one network look (blocking, call it off the UI thread)."""
    release, fresh = cached_release()
    if fresh and not force:
        return release
    found = fetch_latest_release()
    if found is None:
        return release  # offline: keep what we knew, and try again next start
    try:
        UPDATE_CACHE.parent.mkdir(parents=True, exist_ok=True)
        UPDATE_CACHE.write_text(json.dumps({'checked': time.time(), 'release': found}))
    except OSError:
        pass
    return found


def newer_release(installed_version, release):
    """True when `release` is a higher version than the installed build. Unknown on either side: no claim."""
    have, latest = version_key(installed_version), version_key((release or {}).get('version', ''))
    return bool(have and latest and latest > have)


def installed():
    return (ROOT / 'local/nxrt/bin/nxrt').exists() and (ROOT / 'local/install/tomodachi').is_dir()


KEEP_AUTO = 10     # "before playing" copies kept per save
KEEP_POINTS = 20   # automatic save points (taken while playing / at the end of a session) kept per save
KEEP_SAFETY = 5    # "before restore" copies kept per save
MAIN = 'main'      # the save that always existed: local/user-nxrt (never moved)

KIND_NAMES = {'save': 'Backup', 'auto': 'Before playing', 'point': 'Save point', 'session': 'End of session',
              'safety': 'Before restore'}


# --- saves ("islands"): each one is a folder the game uses as its save directory -----------------------------
def profiles_file():
    return ROOT / 'local/profiles.json'


def profiles_dir():
    return ROOT / 'local/profiles'


def _write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_text(json.dumps(data, indent=2, ensure_ascii=False))
    tmp.replace(path)


def load_profiles():
    """{slug: {'name': ..}} in creation order. 'main' (the existing local/user-nxrt) is always there."""
    try:
        data = json.loads(profiles_file().read_text())
        found = {k: v for k, v in data.items() if isinstance(v, dict) and isinstance(v.get('name'), str)}
    except (OSError, ValueError, AttributeError):
        found = {}
    profiles = {MAIN: found.get(MAIN) or {'name': 'Main'}}
    profiles.update({k: v for k, v in found.items() if k != MAIN})
    return profiles


def profile_dir(slug):
    return SAVE_DIR if slug == MAIN else profiles_dir() / slug


def slugify(name):
    slug = re.sub(r'[^a-z0-9]+', '-', name.lower()).strip('-')
    return slug[:30] or 'save'


def create_profile(name):
    """A new, empty save (the game starts a fresh island in it). Returns its slug."""
    name = ' '.join(name.split())[:40]
    if not name:
        raise ValueError('Give the new save a name.')
    profiles = load_profiles()
    if any(p['name'].lower() == name.lower() for p in profiles.values()):
        raise ValueError(f'There is already a save called "{name}".')
    base = slug = slugify(name)
    n = 1
    while slug in profiles or slug == MAIN:
        n += 1
        slug = f'{base}-{n}'
    profiles[slug] = {'name': name, 'created': time.time()}
    profile_dir(slug).mkdir(parents=True, exist_ok=True)
    _write_json(profiles_file(), profiles)
    return slug


def rename_profile(slug, name):
    name = ' '.join(name.split())[:40]
    profiles = load_profiles()
    if slug not in profiles or not name:
        raise ValueError('Give the save a name.')
    if any(k != slug and p['name'].lower() == name.lower() for k, p in profiles.items()):
        raise ValueError(f'There is already a save called "{name}".')
    profiles[slug]['name'] = name
    _write_json(profiles_file(), profiles)


def save_files(slug):
    folder = profile_dir(slug)
    return [f for f in folder.rglob('*') if f.is_file()] if folder.is_dir() else []


def last_activity(slug):
    """When the game last wrote into this save (0 when it has nothing yet)."""
    times = []
    for f in save_files(slug):
        try:
            times.append(f.stat().st_mtime)
        except OSError:
            pass
    return max(times, default=0.0)


def mark_played(slug):
    """Remember that this save was just played (or just imported): the launcher starts on the one played last."""
    profiles = load_profiles()
    if slug in profiles:
        profiles[slug]['played'] = time.time()
        _write_json(profiles_file(), profiles)


def most_recent_profile():
    """The save to start on: the one you played last. A save the game wrote to counts too (playing and closing
    without saving in the game still means you played it). 'main' when nothing has been played or saved yet."""
    profiles = load_profiles()
    def last(k):
        return max(last_activity(k), float(profiles[k].get('played', 0)))
    best = max(profiles, key=lambda k: (last(k), k == MAIN))
    return best if last(best) else MAIN


# --- backups and save points -----------------------------------------------------------------------------
def _index():
    try:
        data = json.loads((BACKUPS / 'index.json').read_text())
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def _remember(name, **info):
    index = _index()
    index[name] = info
    try:
        _write_json(BACKUPS / 'index.json', index)
    except OSError:
        pass


def backup_save(prefix='save', profile=MAIN, label=None):
    """Zip a save into local/backups/ and return the path (None if it has nothing yet). prefix = the kind:
    save (made by hand), auto (before playing), point / session (taken while playing), safety (before a restore)."""
    folder = profile_dir(profile)
    if not folder.is_dir() or not any(folder.iterdir()):
        return None
    BACKUPS.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime('%Y%m%d-%H%M%S')
    stem = f'{prefix}-{stamp}' if profile == MAIN else f'{prefix}-{profile}-{stamp}'
    out, n = BACKUPS / f'{stem}.zip', 1
    while out.exists():  # never overwrite an earlier backup (two in the same second)
        n += 1
        out = BACKUPS / f'{stem}-{n}.zip'
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for path in sorted(folder.rglob('*')):
            if path.is_file():
                z.write(path, path.relative_to(folder).as_posix())
    info = {'kind': prefix, 'profile': profile, 'created': time.time()}
    if label:
        info['label'] = label
    _remember(out.name, **info)
    return out


def list_saves(profile=None):
    """Every backup / save point, newest first: dicts with path, kind, profile, label, created, size.
    Zips made by older launchers (no index entry) are read from their file names."""
    index, out = _index(), []
    for path in BACKUPS.glob('*.zip') if BACKUPS.is_dir() else []:
        info = index.get(path.name)
        if not info:
            kind = path.name.split('-', 1)[0]
            try:
                created = time.mktime(time.strptime(re.search(r'(\d{8}-\d{6})', path.name).group(1), '%Y%m%d-%H%M%S'))
            except (AttributeError, ValueError):
                created = path.stat().st_mtime
            info = {'kind': kind if kind in KIND_NAMES else 'save', 'profile': MAIN, 'created': created}
        if profile is not None and info.get('profile', MAIN) != profile:
            continue
        try:
            size = path.stat().st_size
        except OSError:
            continue
        out.append({'path': path, 'kind': info.get('kind', 'save'), 'profile': info.get('profile', MAIN),
                    'label': info.get('label'), 'created': float(info.get('created', 0)), 'size': size})
    return sorted(out, key=lambda e: e['created'], reverse=True)


def save_title(entry):
    return entry['label'] or KIND_NAMES.get(entry['kind'], 'Backup')


def when_text(created, now=None):
    """'12 min ago', 'Today 17:33', 'Yesterday 21:10', '03 Nov 18:04' (hour:minute so two saves are never confused)."""
    now = time.time() if now is None else now
    age = now - created
    if age < 90:
        return 'just now'
    if age < 3600:
        return f'{int(age // 60)} min ago'
    day = time.strftime('%Y%m%d', time.localtime(created))
    clock = time.strftime('%H:%M', time.localtime(created))
    if day == time.strftime('%Y%m%d', time.localtime(now)):
        return f'Today {clock}'
    if day == time.strftime('%Y%m%d', time.localtime(now - 86400)):
        return f'Yesterday {clock}'
    return time.strftime('%d %b ', time.localtime(created)) + clock


def size_text(size):
    return f'{size / 1048576:.1f} MB' if size >= 1048576 else f'{max(1, round(size / 1024))} KB'


def rename_save(path, label):
    label = ' '.join(label.split())[:50]
    index = _index()
    entry = dict(index.get(Path(path).name) or next(
        ({k: e[k] for k in ('kind', 'profile', 'created')} for e in list_saves() if e['path'] == Path(path)), {}))
    if label:
        entry['label'] = label
    else:
        entry.pop('label', None)
    _remember(Path(path).name, **entry)


def delete_save(path):
    Path(path).unlink()
    index = _index()
    if index.pop(Path(path).name, None) is not None:
        try:
            _write_json(BACKUPS / 'index.json', index)
        except OSError:
            pass


def prune(profile=MAIN):
    """Drop the oldest automatic copies of one save. Backups made by hand and renamed ones are never pruned."""
    caps = {'auto': KEEP_AUTO, 'point': KEEP_POINTS, 'session': KEEP_POINTS, 'safety': KEEP_SAFETY}
    groups = {}
    for e in list_saves(profile):
        if e['kind'] in caps and not e['label']:
            groups.setdefault('points' if e['kind'] in ('point', 'session') else e['kind'], []).append(e)
    for kind, entries in groups.items():
        cap = KEEP_POINTS if kind == 'points' else caps[kind]
        for old in entries[cap:]:  # newest first
            try:
                delete_save(old['path'])
            except OSError:
                pass


def auto_backup(profile=MAIN):
    """Back the save up before each play session and keep only the newest KEEP_AUTO of those (manual ones are never pruned)."""
    try:
        out = backup_save('auto', profile)
        prune(profile)
        return out
    except OSError:
        return None  # a full disk must not stop the game from starting


def restore_save(archive, profile=MAIN):
    """Replace a save with the contents of `archive`, after backing the current one up. Returns the safety backup path."""
    folder = profile_dir(profile)
    with zipfile.ZipFile(archive) as z:
        bad = [n for n in z.namelist() if n.startswith('/') or '..' in Path(n).parts]
        if bad:
            raise ValueError('The zip has unsafe paths; refusing to restore it.')
        safety = backup_save('safety', profile)
        if folder.exists():
            shutil.rmtree(folder)
        folder.mkdir(parents=True)
        z.extractall(folder)
    prune(profile)
    return safety


# --- watching a save while the game runs ----------------------------------------------------------------
def save_signature(profile=MAIN):
    """Cheap fingerprint of a save folder: it changes whenever the game writes into it."""
    sig = []
    for f in sorted(save_files(profile)):
        try:
            st = f.stat()
            sig.append((f.name, st.st_size, st.st_mtime_ns))
        except OSError:
            pass
    return tuple(sig)


class SaveWatch:
    """Takes a save point when the game has written a save and then gone quiet, and tells at the end of the session
    what was (not) saved. The game keeps its progress in memory until you save in the game: a copy of the folder
    cannot contain what was never written, so this protects what the game DID write (a half-written save after a
    kill, an overwrite you regret) and says plainly when the last real save was."""
    QUIET = 4.0       # seconds without writes before a change counts as a finished save
    MIN_GAP = 180.0   # at most one automatic save point per 3 minutes
    HURT = 10.0       # a kill this soon after a write may have cut it in half

    def __init__(self, profile=MAIN, now=None):
        now = time.time() if now is None else now
        self.profile, self.started = profile, now
        self.sig = save_signature(profile)
        self.dirty, self.changed_at, self.last_write = False, None, None
        self.last_point = now - self.MIN_GAP
        self.points = 0

    def poll(self, now=None):
        """Call every couple of seconds. Returns the path of a new save point, or None."""
        now = time.time() if now is None else now
        sig = save_signature(self.profile)
        if sig != self.sig:
            self.sig, self.dirty = sig, True
            self.changed_at = self.last_write = now
            return None
        if self.dirty and now - self.changed_at >= self.QUIET and now - self.last_point >= self.MIN_GAP:
            return self._take('point', now)
        return None

    def _take(self, kind, now):
        try:
            out = backup_save(kind, self.profile)
            prune(self.profile)
        except OSError:
            return None
        self.dirty, self.last_point = False, now
        self.points += 1 if out else 0
        return out

    def finish(self, exit_code, now=None):
        """The game ended. Returns a dict for the window: text (a sentence to show), interrupted, saved, last_write."""
        now = time.time() if now is None else now
        killed = exit_code is not None and (exit_code < 0 or exit_code >= 128)
        interrupted = bool(self.dirty and self.changed_at is not None and (killed or exit_code not in (0, None))
                           and now - self.changed_at < self.HURT)
        saved = None
        if self.dirty and not interrupted:
            saved = self._take('session', now)
        minutes = None if self.last_write is None else (now - self.last_write) / 60
        played = (now - self.started) / 60
        if interrupted:
            text = ('The game was closed while it was writing the save. If it misbehaves, restore the previous '
                    'save point from "All saves".')
        elif self.last_write is None:
            text = ('Nothing was saved this session.' if played >= 5 else '')
        elif minutes >= 10:
            text = f'Your last save in the game was {int(minutes)} min before you closed it. Anything after it is lost.'
        else:
            text = 'Saved. Have a nice day on the island!'
        return {'text': text, 'interrupted': interrupted, 'saved': saved, 'last_write': self.last_write,
                'minutes_since_save': minutes}


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
    try:
        launcher_ui.run(sys.modules[__name__], smoke='--smoke' in sys.argv)
    except Exception:  # noqa: BLE001 - whatever went wrong, the game must stay reachable
        import traceback
        traceback.print_exc()
        print('\nThe launcher could not start (the error is above). You can still play: ./play.sh --nogui\n'
              'If you report this, attach the text above.', file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    try:
        main()
    except ImportError:
        print('The launcher needs Tk (python3-tk). Start the game with ./play.sh --nogui instead.')
        sys.exit(1)
