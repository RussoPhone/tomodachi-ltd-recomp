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


if __name__ == '__main__':
    unittest.main()
