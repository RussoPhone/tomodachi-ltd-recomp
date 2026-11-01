#!/usr/bin/env python3
"""Launcher for the unofficial native port of Tomodachi Life: Living the Dream (nxrt).

One window with PLAY, the settings that matter (fullscreen, window size, language), save backup/restore and a bug-report button.
Settings are kept in local/launcher.json and handed to scripts/play-nxrt.sh as options. Needs only Python's built-in Tk.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time
import webbrowser
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


def backup_save():
    """Zip local/user-nxrt into local/backups/save-<time>.zip and return the path (None if there is no save yet)."""
    if not SAVE_DIR.is_dir() or not any(SAVE_DIR.iterdir()):
        return None
    BACKUPS.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime('%Y%m%d-%H%M%S')
    out, n = BACKUPS / f'save-{stamp}.zip', 1
    while out.exists():  # never overwrite an earlier backup (two in the same second)
        n += 1
        out = BACKUPS / f'save-{stamp}-{n}.zip'
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for path in sorted(SAVE_DIR.rglob('*')):
            if path.is_file():
                z.write(path, path.relative_to(SAVE_DIR).as_posix())
    return out


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
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    settings = load_settings()
    root = tk.Tk()
    root.title('Tomodachi Life (unofficial native port, alpha)')
    root.geometry('660x600')
    root.minsize(560, 480)
    frame = ttk.Frame(root, padding=16)
    frame.pack(fill='both', expand=True)

    ttk.Label(frame, text='Tomodachi Life: Living the Dream', font=('TkDefaultFont', 15, 'bold')).pack(anchor='w')
    ttk.Label(frame, text='Unofficial native PC port · alpha · runs from your own copy of the game').pack(anchor='w')

    status = tk.StringVar()
    ttk.Label(frame, textvariable=status, foreground='#555').pack(anchor='w', pady=(8, 0))

    play = ttk.Button(frame, text='PLAY')
    play.pack(fill='x', pady=10, ipady=8)

    box = ttk.LabelFrame(frame, text='Settings', padding=10)
    box.pack(fill='x')
    fullscreen = tk.BooleanVar(value=settings['fullscreen'])
    safe = tk.BooleanVar(value=settings.get('safe', False))
    size = tk.StringVar(value=settings['size'])
    language = tk.StringVar(value=next((n for n, c in LANGUAGES if c == settings['language']), LANGUAGES[0][0]))
    ttk.Checkbutton(box, text='Fullscreen (F11 also toggles it in the game)', variable=fullscreen).grid(
        row=0, column=0, columnspan=2, sticky='w')
    ttk.Checkbutton(box, text='Safe mode (use it if the picture looks wrong: slower, but fewer things can go wrong)',
                    variable=safe).grid(row=4, column=0, columnspan=2, sticky='w', pady=(6, 0))
    ttk.Label(box, text='Window size').grid(row=1, column=0, sticky='w', pady=(6, 0))
    ttk.Combobox(box, textvariable=size, values=SIZES, width=14).grid(row=1, column=1, sticky='w', pady=(6, 0))
    ttk.Label(box, text='Language').grid(row=2, column=0, sticky='w', pady=(6, 0))
    ttk.Combobox(box, textvariable=language, values=[n for n, _ in LANGUAGES], width=28, state='readonly').grid(
        row=2, column=1, sticky='w', pady=(6, 0))
    ttk.Label(box, text='These options are new and only lightly tested. The first time you open the face-paint editor it takes\n'
                        'about 40 seconds (it builds a cache); after that it opens in seconds.', foreground='#555').grid(
        row=3, column=0, columnspan=2, sticky='w', pady=(8, 0))

    saves = ttk.LabelFrame(frame, text='Save data', padding=10)
    saves.pack(fill='x', pady=10)

    help_box = ttk.LabelFrame(frame, text='Help', padding=10)
    help_box.pack(fill='x')

    log_var = tk.StringVar(value='')
    ttk.Label(frame, textvariable=log_var, foreground='#555', wraplength=580, justify='left').pack(anchor='w', pady=(10, 0))

    def current_settings():
        code = next((c for n, c in LANGUAGES if n == language.get()), 'en-US')
        value = size.get().strip().lower()
        if 'x' not in value or not all(p.isdigit() for p in value.split('x', 1)):
            value = DEFAULTS['size']
        return {'fullscreen': fullscreen.get(), 'size': value, 'language': code, 'safe': safe.get()}

    def refresh_status():
        if installed():
            status.set('Ready to play.')
            play.state(['!disabled'])
        else:
            status.set('The native port is not installed yet. Run ./install.sh first.')
            play.state(['disabled'])

    def on_play():
        current = current_settings()
        save_settings(current)
        play.state(['disabled'])
        status.set('Playing… close the game window to come back.')
        root.iconify()

        def run():
            try:
                code = subprocess.run(play_command(current), cwd=ROOT).returncode
                note = '' if code == 0 else f'The game ended with code {code}. See local/logs/play-nxrt-*.log.'
            except OSError as error:
                note = f'Could not start the game: {error}'
            root.after(0, lambda: (root.deiconify(), refresh_status(), log_var.set(note)))

        threading.Thread(target=run, daemon=True).start()

    def on_backup():
        out = backup_save()
        log_var.set(f'Backup saved: {out.relative_to(ROOT)}' if out else 'There is no save to back up yet.')

    def on_restore():
        archive = filedialog.askopenfilename(title='Choose a save backup (.zip)', initialdir=BACKUPS if BACKUPS.exists() else ROOT,
                                             filetypes=[('Backups', '*.zip')])
        if not archive:
            return
        if not messagebox.askyesno('Restore save', 'This replaces your current save with the backup.\n'
                                   'The current save is backed up first. Continue?'):
            return
        try:
            safety = restore_save(archive)
        except (OSError, ValueError, zipfile.BadZipFile) as error:
            messagebox.showerror('Restore failed', str(error))
            return
        log_var.set('Save restored.' + (f' Previous save kept in {safety.relative_to(ROOT)}.' if safety else ''))

    def on_check():
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/doctor.py')], cwd=ROOT, capture_output=True, text=True)
        messagebox.showinfo('Check my computer', (result.stdout or result.stderr).strip() or 'No output.')

    def on_report():
        log_var.set('Collecting logs…')
        root.update_idletasks()
        out = make_report()
        log_var.set(f'Bug report saved: {out.name} (no game data or keys inside). Attach it to a GitHub issue.' if out
                    else 'Could not create the report. Run ./install.sh --report in a terminal.')

    play.configure(command=on_play)
    ttk.Button(saves, text='Back up now', command=on_backup).pack(side='left')
    ttk.Button(saves, text='Restore…', command=on_restore).pack(side='left', padx=6)
    ttk.Button(saves, text='Open save folder', command=lambda: open_folder(SAVE_DIR)).pack(side='left')
    ttk.Button(help_box, text='Check my computer', command=on_check).pack(side='left')
    ttk.Button(help_box, text='Create bug report', command=on_report).pack(side='left', padx=6)
    ttk.Button(help_box, text='Open logs', command=lambda: open_folder(LOGS)).pack(side='left')
    ttk.Button(help_box, text='Testing guide', command=lambda: webbrowser.open(TESTING_URL)).pack(side='left', padx=6)
    ttk.Button(help_box, text='Report a problem', command=lambda: webbrowser.open(ISSUES_URL)).pack(side='left')

    refresh_status()
    if '--smoke' in sys.argv:  # used by the tests: build the window, then close it
        root.after(200, root.destroy)
    root.mainloop()


if __name__ == '__main__':
    try:
        main()
    except ImportError:
        print('The launcher needs Tk (python3-tk). Start the game with ./play.sh --nogui instead.')
        sys.exit(1)
