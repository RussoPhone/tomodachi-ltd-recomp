import importlib.util
import os
import subprocess
import shutil
import io
import json
import time
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/launcher.py'
spec = importlib.util.spec_from_file_location('launcher', SCRIPT)
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        launcher.SETTINGS = self.tmp / 'launcher.json'
        launcher.SAVE_DIR = self.tmp / 'user-nxrt'
        launcher.BACKUPS = self.tmp / 'backups'
        launcher.ROOT = self.tmp
        self._urlopen = launcher.urllib.request.urlopen
        launcher.UPDATE_CACHE = self.tmp / 'update-check.json'
        self.addCleanup(lambda: setattr(launcher.urllib.request, 'urlopen', self._urlopen))

    def test_settings_roundtrip_and_defaults(self):
        self.assertEqual(launcher.load_settings(), launcher.DEFAULTS)
        launcher.save_settings({'fullscreen': True, 'size': '1920x1080', 'language': 'ja'})
        self.assertEqual(launcher.load_settings()['language'], 'ja')
        launcher.SETTINGS.write_text('not json')
        self.assertEqual(launcher.load_settings(), launcher.DEFAULTS)

    def test_play_command(self):
        cmd = launcher.play_command({'fullscreen': True, 'size': '1920x1080', 'language': 'en-GB'})
        self.assertEqual(cmd[1:], ['--language', 'en-GB', '--size', '1920x1080', '--fullscreen'])
        safe = launcher.play_command({'fullscreen': False, 'size': '1280x720', 'language': 'en-US', 'safe': True})
        self.assertIn('--safe', safe)

    def test_auto_backup_keeps_only_the_newest_copies(self):
        (launcher.SAVE_DIR).mkdir(parents=True, exist_ok=True)
        (launcher.SAVE_DIR / 'a.sav').write_bytes(b'x')
        for _ in range(launcher.KEEP_AUTO + 3):
            launcher.auto_backup()
        self.assertEqual(len(list(launcher.BACKUPS.glob('auto-*.zip'))), launcher.KEEP_AUTO)

    def test_backup_and_restore_keep_a_safety_copy(self):
        self.assertIsNone(launcher.backup_save())
        launcher.SAVE_DIR.mkdir()
        (launcher.SAVE_DIR / 'a.sav').write_bytes(b'one')
        first = launcher.backup_save()
        (launcher.SAVE_DIR / 'a.sav').write_bytes(b'two')
        safety = launcher.restore_save(first)
        self.assertEqual((launcher.SAVE_DIR / 'a.sav').read_bytes(), b'one')
        with zipfile.ZipFile(safety) as z:
            self.assertEqual(z.read('a.sav'), b'two')

    def test_restore_refuses_unsafe_paths(self):
        evil = self.tmp / 'evil.zip'
        with zipfile.ZipFile(evil, 'w') as z:
            z.writestr('../outside.txt', 'x')
        launcher.SAVE_DIR.mkdir()
        (launcher.SAVE_DIR / 'keep.sav').write_bytes(b'keep')
        with self.assertRaises(ValueError):
            launcher.restore_save(evil)
        self.assertTrue((launcher.SAVE_DIR / 'keep.sav').exists())


    def test_desktop_entry_is_found_by_search_and_quotes_paths(self):
        launcher.ROOT = self.tmp / 'my games' / 'tomodachi ltd recomp'
        (launcher.ROOT / 'assets').mkdir(parents=True)
        (launcher.ROOT / 'assets/icon.svg').write_text('<svg/>')
        entry = launcher.desktop_entry()
        self.assertIn('Name=Tomodachi-LTD-Recomp', entry)
        self.assertIn('Tomodachi LTD Recomp', next(l for l in entry.splitlines() if l.startswith('Keywords=')))
        self.assertIn(f'Exec="{launcher.ROOT}/play.sh"', entry)  # quoted: the folder has spaces
        self.assertIn(f'Path={launcher.ROOT}', entry)
        self.assertIn('Terminal=false', entry)
        self.assertIn(f'Icon={launcher.ROOT}/assets/icon.svg', entry)

    def test_install_and_remove_shortcut_replace_the_old_entry(self):
        apps = self.tmp / 'apps'
        apps.mkdir()
        (apps / launcher.OLD_DESKTOP_FILE).write_text(f'[Desktop Entry]\nExec={launcher.ROOT}/play.sh\n')
        other = apps / 'other.desktop'
        other.write_text('x')
        out = launcher.install_shortcut(apps)
        self.assertEqual(out, apps / launcher.DESKTOP_FILE)
        self.assertTrue(out.exists())
        self.assertFalse((apps / launcher.OLD_DESKTOP_FILE).exists())  # same folder: replaced, not duplicated
        self.assertTrue(other.exists())
        self.assertEqual(launcher.remove_shortcut(apps), [out])
        self.assertFalse(out.exists())
        self.assertTrue(other.exists())

    def test_install_shortcut_keeps_the_old_entry_of_another_folder(self):
        apps = self.tmp / 'apps'
        apps.mkdir()
        old = apps / launcher.OLD_DESKTOP_FILE
        old.write_text('[Desktop Entry]\nExec=/somewhere/else/play.sh\n')
        launcher.install_shortcut(apps)
        self.assertTrue(old.exists())

    def test_languages_are_the_ones_the_game_ships_and_the_runtime_knows(self):
        # romfs/Mals has message data for exactly these 13 (USen EUen JPja EUfr USfr EUde EUit EUes USes EUnl KRko CNzh TWzh)
        self.assertEqual(sorted(launcher.LANGUAGE_CODES), sorted(
            ['en-US', 'en-GB', 'ja', 'fr', 'fr-CA', 'de', 'it', 'es', 'es-419', 'nl', 'ko', 'zh-Hans', 'zh-Hant']))
        runtime = (Path(__file__).resolve().parents[1] / 'runtime/src/services/am.cpp').read_text()
        for code in launcher.LANGUAGE_CODES:  # every offered code must be one GetDesiredLanguage accepts
            self.assertIn(f'"{code}"', runtime, code)

    def test_a_language_the_game_has_no_text_for_falls_back_to_english(self):
        for old in ('pt-BR', 'ru', 'nonsense'):
            launcher.SETTINGS.write_text('{"language": "%s", "size": "1920x1080"}' % old)
            settings = launcher.load_settings()
            self.assertEqual(settings['language'], 'en-US')
            self.assertEqual(launcher.LANGUAGE_RESET, old)
            self.assertEqual(settings['size'], '1920x1080')
        launcher.SETTINGS.write_text('{"language": "ja"}')
        self.assertEqual(launcher.load_settings()['language'], 'ja')
        self.assertIsNone(launcher.LANGUAGE_RESET)

    def _fake_build(self, text, exit_code=2):
        binary = self.tmp / 'local/nxrt/bin/nxrt'
        binary.parent.mkdir(parents=True, exist_ok=True)
        binary.write_text(f'#!/bin/sh\necho "{text}" >&2\nexit {exit_code}\n')
        binary.chmod(0o755)

    def test_build_version_comes_from_the_installed_binary(self):
        self.assertEqual(launcher.build_version(), '')  # no build installed
        self._fake_build('[    0.000] info  nxrt Alpha 1.7.0')
        self.assertEqual(launcher.build_version(), 'Alpha 1.7.0')
        self._fake_build('[    0.000] info  nxrt Alpha 9.8.7-rc1')
        self.assertEqual(launcher.build_version(), 'Alpha 9.8.7-rc1')
        self._fake_build('[    0.000] info  nxrt 2.0.1')
        self.assertEqual(launcher.build_version(), '2.0.1')

    def test_build_version_is_empty_rather_than_guessed(self):
        self._fake_build('something else entirely')
        self.assertEqual(launcher.build_version(), '')
        (self.tmp / 'CHANGELOG.md').write_text('## Alpha 9.9.9 - 2030-01-01\n')  # the source's version must not leak in
        self.assertEqual(launcher.build_version(), '')

    def _github(self, releases):
        class Response(io.BytesIO):
            __enter__ = lambda self: self
            __exit__ = lambda self, *a: None
        launcher.urllib.request.urlopen = lambda request, timeout=None: Response(json.dumps(releases).encode())

    def test_version_key_reads_the_numbers_whatever_the_wrapping(self):
        self.assertEqual(launcher.version_key('Alpha 1.7.4'), (1, 7, 4))
        self.assertEqual(launcher.version_key('v1.7.4'), (1, 7, 4))
        self.assertEqual(launcher.version_key('1.10.0-rc1'), (1, 10, 0))
        self.assertGreater(launcher.version_key('Alpha 1.10.0'), launcher.version_key('Alpha 1.9.9'))  # not a string compare
        self.assertEqual(launcher.version_key('nothing'), ())

    def test_latest_release_is_the_highest_version_not_the_first_listed(self):
        self._github([
            {'tag_name': 'v1.7.2', 'name': 'Alpha 1.7.2: mouse', 'html_url': 'u2', 'prerelease': True},
            {'tag_name': 'v1.7.4', 'name': 'Alpha 1.7.4: the clock is right again', 'html_url': 'u4', 'prerelease': True},
            {'tag_name': 'v1.8.0', 'name': 'Alpha 1.8.0: draft', 'html_url': 'u8', 'draft': True},  # drafts do not count
            {'tag_name': 'v1.7.3', 'name': 'Alpha 1.7.3', 'html_url': 'u3'}])
        latest = launcher.fetch_latest_release()
        self.assertEqual((latest['version'], latest['url'], latest['title']),
                         ('Alpha 1.7.4', 'u4', 'the clock is right again'))

    def test_a_failed_or_odd_answer_is_no_release_not_an_error(self):
        def boom(request, timeout=None):
            raise OSError('offline')
        launcher.urllib.request.urlopen = boom
        self.assertIsNone(launcher.fetch_latest_release())
        self._github({'message': 'API rate limit exceeded'})
        self.assertIsNone(launcher.fetch_latest_release())
        self._github([])
        self.assertIsNone(launcher.fetch_latest_release())

    def test_update_check_uses_the_cache_and_survives_being_offline(self):
        launcher.UPDATE_CACHE = self.tmp / 'update-check.json'
        calls = []
        self._github([{'tag_name': 'v1.7.4', 'name': 'Alpha 1.7.4', 'html_url': 'u'}])
        original = launcher.urllib.request.urlopen
        launcher.urllib.request.urlopen = lambda *a, **k: (calls.append(1), original(*a, **k))[1]
        self.assertEqual(launcher.check_for_update()['version'], 'Alpha 1.7.4')
        self.assertEqual(launcher.check_for_update()['version'], 'Alpha 1.7.4')
        self.assertEqual(len(calls), 1)  # the second start used the cache
        launcher.UPDATE_CACHE.write_text(json.dumps({'checked': time.time() - 7 * 3600, 'release': {'version': 'Alpha 1.7.3'}}))
        def boom(request, timeout=None):
            raise OSError('offline')
        launcher.urllib.request.urlopen = boom
        self.assertEqual(launcher.check_for_update()['version'], 'Alpha 1.7.3')  # stale but better than nothing

    def test_newer_release_only_claims_what_it_knows(self):
        latest = {'version': 'Alpha 1.7.4'}
        self.assertTrue(launcher.newer_release('Alpha 1.7.0', latest))
        self.assertFalse(launcher.newer_release('Alpha 1.7.4', latest))
        self.assertFalse(launcher.newer_release('Alpha 1.8.0', latest))  # a dev build ahead of the release
        self.assertFalse(launcher.newer_release('', latest))             # installed version unknown: no claim
        self.assertFalse(launcher.newer_release('Alpha 1.7.0', None))

    # --- saves ("islands") ---------------------------------------------------------------------------
    def _write_save(self, slug, content=b'x', mtime=None):
        folder = launcher.profile_dir(slug)
        folder.mkdir(parents=True, exist_ok=True)
        f = folder / 'game.sav'
        f.write_bytes(content)
        if mtime:
            os.utime(f, (mtime, mtime))
        return f

    def _fake_zip(self, name, kind, created, label=None, profile='main'):
        launcher.BACKUPS.mkdir(parents=True, exist_ok=True)
        path = launcher.BACKUPS / name
        with zipfile.ZipFile(path, 'w') as z:
            z.writestr('game.sav', name)
        info = {'kind': kind, 'profile': profile, 'created': created}
        if label:
            info['label'] = label
        launcher._remember(name, **info)
        return path

    def test_the_main_save_is_always_there_and_new_saves_are_separate_folders(self):
        self.assertEqual(list(launcher.load_profiles()), ['main'])
        self.assertEqual(launcher.profile_dir('main'), launcher.SAVE_DIR)  # the existing save is never moved
        slug = launcher.create_profile('  My   Island! ')
        self.assertEqual(launcher.load_profiles()[slug]['name'], 'My Island!')
        self.assertEqual(launcher.profile_dir(slug), self.tmp / 'local/profiles' / slug)
        self.assertTrue(launcher.profile_dir(slug).is_dir())
        for bad in ('my island!', 'MAIN', 'Main', ''):  # same name, whatever the case; the empty name
            with self.assertRaises(ValueError):
                launcher.create_profile(bad)
        self.assertEqual(launcher.create_profile('My Island?'.replace('?', '2')), slug + '2')  # a slug is never reused

    def test_rename_a_save_keeps_names_unique(self):
        slug = launcher.create_profile('Practice')
        launcher.rename_profile(slug, 'Real game')
        self.assertEqual(launcher.load_profiles()[slug]['name'], 'Real game')
        with self.assertRaises(ValueError):
            launcher.rename_profile(slug, 'main')

    def test_the_launcher_starts_on_the_save_the_game_wrote_to_last(self):
        self.assertEqual(launcher.most_recent_profile(), 'main')  # nothing yet
        other = launcher.create_profile('Other')
        self._write_save('main', mtime=1000)
        self._write_save(other, mtime=2000)
        self.assertEqual(launcher.most_recent_profile(), other)
        self._write_save('main', mtime=3000)
        self.assertEqual(launcher.most_recent_profile(), 'main')

    def test_the_save_you_played_last_wins_even_if_it_was_not_written(self):
        other = launcher.create_profile('Other')
        self._write_save('main', mtime=5000)            # main was written most recently...
        self._write_save(other, mtime=1000)
        self.assertEqual(launcher.most_recent_profile(), 'main')
        launcher.mark_played(other)                     # ...but the other one was played after that
        self.assertEqual(launcher.most_recent_profile(), other)
        launcher.mark_played('nope')                    # an unknown save is ignored, not created
        self.assertEqual(list(launcher.load_profiles()), ['main', other])

    def test_play_command_passes_the_save_folder_only_when_it_is_not_the_default(self):
        settings = {'fullscreen': False, 'size': '1280x720', 'language': 'en-US'}
        self.assertNotIn('--user', launcher.play_command(settings, launcher.SAVE_DIR))  # works with an older play-nxrt.sh
        cmd = launcher.play_command(settings, self.tmp / 'x')
        self.assertEqual(cmd[cmd.index('--user') + 1], str(self.tmp / 'x'))

    def test_backups_get_readable_titles_and_can_be_named_renamed_and_deleted(self):
        self._write_save('main')
        point = launcher.backup_save('point')
        named = launcher.backup_save('save', 'main', 'Before the festival')
        entries = launcher.list_saves('main')
        self.assertEqual({launcher.save_title(e) for e in entries}, {'Save point', 'Before the festival'})
        launcher.rename_save(point, 'After the first flower')
        self.assertIn('After the first flower', [launcher.save_title(e) for e in launcher.list_saves()])
        launcher.rename_save(point, '')  # an empty name goes back to the kind
        self.assertIn('Save point', [launcher.save_title(e) for e in launcher.list_saves()])
        launcher.delete_save(named)
        self.assertFalse(named.exists())
        self.assertNotIn(named.name, launcher._index())

    def test_zips_from_older_launchers_are_still_listed(self):
        launcher.BACKUPS.mkdir(parents=True)
        for name in ('save-20261105-173346.zip', 'auto-20261105-044933.zip'):
            with zipfile.ZipFile(launcher.BACKUPS / name, 'w') as z:
                z.writestr('a', 'b')
        entries = launcher.list_saves('main')
        self.assertEqual([launcher.save_title(e) for e in entries], ['Backup', 'Before playing'])  # newest first
        self.assertEqual(time.strftime('%Y-%m-%d %H:%M', time.localtime(entries[0]['created'])), '2026-11-05 17:33')

    def test_when_and_size_are_short_and_human(self):
        now = time.mktime((2026, 11, 5, 18, 0, 0, 0, 0, -1))
        at = lambda d, h, m: time.mktime((2026, 11, d, h, m, 0, 0, 0, -1))
        self.assertEqual(launcher.when_text(now - 30, now), 'just now')
        self.assertEqual(launcher.when_text(now - 12 * 60, now), '12 min ago')
        self.assertEqual(launcher.when_text(at(5, 15, 7), now), 'Today 15:07')
        self.assertEqual(launcher.when_text(at(4, 21, 10), now), 'Yesterday 21:10')
        older = launcher.when_text(at(3, 18, 4), now)
        self.assertTrue(older.startswith('03 ') and older.endswith('18:04'), older)
        self.assertEqual(launcher.size_text(500), '1 KB')
        self.assertEqual(launcher.size_text(56 * 1024), '56 KB')
        self.assertEqual(launcher.size_text(int(1.5 * 1048576)), '1.5 MB')

    def test_prune_drops_old_automatic_copies_but_never_named_ones_or_other_saves(self):
        other = launcher.create_profile('Other')
        base = 1_000_000
        for i in range(launcher.KEEP_POINTS + 5):
            self._fake_zip(f'point-{i:03}.zip', 'point', base + i)
        named = self._fake_zip('point-old.zip', 'point', base - 10, label='Keep me')
        manual = self._fake_zip('save-old.zip', 'save', base - 20)
        theirs = self._fake_zip('point-other.zip', 'point', base - 30, profile=other)
        launcher.prune('main')
        kept = [e['path'].name for e in launcher.list_saves('main') if e['kind'] == 'point' and not e['label']]
        self.assertEqual(len(kept), launcher.KEEP_POINTS)
        self.assertIn(f'point-{launcher.KEEP_POINTS + 4:03}.zip', kept)       # the newest stay
        self.assertNotIn('point-000.zip', kept)                               # the oldest went
        self.assertTrue(named.exists() and manual.exists() and theirs.exists())

    def test_restoring_one_save_leaves_the_others_alone_and_keeps_a_safety_copy(self):
        other = launcher.create_profile('Other')
        self._write_save('main', b'main-now')
        self._write_save(other, b'other-then')
        snap = launcher.backup_save('save', other)
        self._write_save(other, b'other-now')
        safety = launcher.restore_save(snap, other)
        self.assertEqual((launcher.profile_dir(other) / 'game.sav').read_bytes(), b'other-then')
        self.assertEqual((launcher.SAVE_DIR / 'game.sav').read_bytes(), b'main-now')
        with zipfile.ZipFile(safety) as z:
            self.assertEqual(z.read('game.sav'), b'other-now')
        kinds = {(e['kind'], e['profile']) for e in launcher.list_saves()}
        self.assertIn(('safety', other), kinds)

    # --- watching a save while the game runs -------------------------------------------------------------
    def _touch(self, content, slug='main'):
        self._write_save(slug, content)

    def test_a_save_point_is_taken_when_the_game_wrote_and_went_quiet(self):
        self._touch(b'start')
        watch = launcher.SaveWatch('main', now=1000)
        self.assertIsNone(watch.poll(1001))                 # nothing changed
        self._touch(b'the player saved in the game')
        self.assertIsNone(watch.poll(1002))                 # just written: it may still be writing
        self.assertIsNone(watch.poll(1004))
        point = watch.poll(1007)                            # quiet for 5 s: the save is complete
        self.assertIsNotNone(point)
        self.assertEqual([e['kind'] for e in launcher.list_saves('main')], ['point'])
        self.assertIsNone(watch.poll(1010))                 # nothing new, nothing more

    def test_save_points_are_spaced_out(self):
        self._touch(b'1')
        watch = launcher.SaveWatch('main', now=1000)
        watch.poll(1001)
        self._touch(b'22')
        watch.poll(1002)
        self.assertIsNotNone(watch.poll(1007))
        self._touch(b'333')
        watch.poll(1020)
        self.assertIsNone(watch.poll(1030))                 # only 23 s after the last point: wait
        self.assertIsNotNone(watch.poll(1007 + launcher.SaveWatch.MIN_GAP + 1))

    def test_a_normal_close_after_a_save_says_saved_and_keeps_the_final_state(self):
        self._touch(b'a')
        watch = launcher.SaveWatch('main', now=1000)
        self._touch(b'bb')
        watch.poll(1001)
        done = watch.finish(0, now=1100)                    # Alt+F4 and a normal exit are both 0
        self.assertFalse(done['interrupted'])
        self.assertIsNotNone(done['saved'])                 # the last state was kept as an "end of session" point
        self.assertIn('Saved', done['text'])

    def test_it_says_how_long_before_closing_the_last_save_was(self):
        self._touch(b'a')
        watch = launcher.SaveWatch('main', now=1000)
        self._touch(b'bb')
        watch.poll(1001)
        done = watch.finish(0, now=1001 + 47 * 60)
        self.assertIn('47 min', done['text'])
        self.assertIn('lost', done['text'])

    def test_a_session_without_a_save_says_so_only_when_it_was_long_enough_to_matter(self):
        self._touch(b'a')
        self.assertEqual(launcher.SaveWatch('main', now=1000).finish(0, now=1060)['text'], '')
        self.assertEqual(launcher.SaveWatch('main', now=1000).finish(0, now=1000 + 30 * 60)['text'],
                         'Nothing was saved this session.')

    def test_a_kill_right_after_a_write_is_reported_and_not_kept_as_a_save_point(self):
        self._touch(b'a')
        watch = launcher.SaveWatch('main', now=1000)
        self._touch(b'half-written')
        watch.poll(1001)
        done = watch.finish(137, now=1003)                  # killed 2 s after the game wrote
        self.assertTrue(done['interrupted'])
        self.assertIsNone(done['saved'])                    # a possibly half-written state is not a "good" point
        self.assertIn('writing the save', done['text'])
        self.assertEqual(launcher.list_saves('main'), [])

    def test_a_kill_long_after_the_last_write_is_not_called_interrupted(self):
        self._touch(b'a')
        watch = launcher.SaveWatch('main', now=1000)
        self._touch(b'bb')
        watch.poll(1001)
        watch.poll(1010)                                    # the save point was taken
        done = watch.finish(137, now=1600)
        self.assertFalse(done['interrupted'])

    # --- the play script: --user and the game's exit code through the pipe ------------------------------
    def _run_play_script(self, *args, game_exit=0):
        root = self.tmp / 'game-root'
        (root / 'scripts').mkdir(parents=True, exist_ok=True)
        (root / 'local/nxrt/bin').mkdir(parents=True, exist_ok=True)
        (root / 'local/install/tomodachi').mkdir(parents=True, exist_ok=True)
        shutil.copy(Path(__file__).resolve().parents[1] / 'scripts/play-nxrt.sh', root / 'scripts/play-nxrt.sh')
        nxrt = root / 'local/nxrt/bin/nxrt'
        nxrt.write_text('#!/bin/sh\necho "args: $*"\nexit ' + str(game_exit) + '\n')
        nxrt.chmod(0o755)
        exit_file = self.tmp / 'exit.txt'
        env = dict(os.environ, NXRT_EXIT_FILE=str(exit_file))
        done = subprocess.run(['sh', str(root / 'scripts/play-nxrt.sh'), *args], cwd=root, env=env, capture_output=True, text=True)
        return root, done, exit_file

    def test_play_script_hands_back_the_games_exit_code(self):
        for code in (0, 3, 137):
            root, done, exit_file = self._run_play_script(game_exit=code)
            self.assertEqual(done.returncode, 0)             # the script itself keeps exiting 0 (as before)
            self.assertEqual(exit_file.read_text().strip(), str(code))

    def test_play_script_uses_the_given_save_folder(self):
        other = self.tmp / 'my-island'
        root, done, _ = self._run_play_script('--user', str(other))
        self.assertTrue(other.is_dir())
        self.assertFalse((root / 'local/user-nxrt').exists())
        root, done, _ = self._run_play_script()
        self.assertTrue((root / 'local/user-nxrt').is_dir())  # the default is unchanged

    def test_a_launcher_that_cannot_start_says_how_to_play_anyway(self):
        """No display (or no Tk): a clear message and a non-zero exit, never a silent crash."""
        env = {k: v for k, v in os.environ.items() if k != 'WAYLAND_DISPLAY'}
        env['DISPLAY'] = ':99'  # nothing listens there
        done = subprocess.run([sys.executable, str(Path(__file__).resolve().parents[1] / 'scripts/launcher.py')],
                              env=env, capture_output=True, text=True, timeout=60)
        self.assertEqual(done.returncode, 1)
        self.assertIn('./play.sh --nogui', done.stderr)


if __name__ == '__main__':
    unittest.main()
