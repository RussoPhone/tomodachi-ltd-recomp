import importlib.util
from pathlib import Path
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

    def test_settings_roundtrip_and_defaults(self):
        self.assertEqual(launcher.load_settings(), launcher.DEFAULTS)
        launcher.save_settings({'fullscreen': True, 'size': '1920x1080', 'language': 'pt-BR'})
        self.assertEqual(launcher.load_settings()['language'], 'pt-BR')
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


if __name__ == '__main__':
    unittest.main()
