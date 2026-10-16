"""Exercise generated launchers and package descriptions without game assets."""
import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/assemble-package.py'
spec = importlib.util.spec_from_file_location('tested_packaging', SCRIPT)
packaging = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packaging)


class PackagingTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / 'local').mkdir()
        (self.root / 'local/runtime.json').write_text('{"target":"tomodachi"}')
        adapter = self.root / 'adapters/tomodachi/target.json'
        adapter.parent.mkdir(parents=True)
        adapter.write_text('{"name":"Test Title","expected_title_id":"0123456789ABCDEF"}')
        exe = self.root / 'upstream/mk8-recomp/build/suyu-static/bin/suyu-cmd-static'
        exe.parent.mkdir(parents=True)
        exe.write_bytes(b'fake executable: not playable')
        romfs = self.root / 'local/install/tomodachi/exefs/romfs.bin'
        romfs.parent.mkdir(parents=True)
        romfs.write_bytes(b'fake payload')
        for module, key in [(packaging, 'ROOT'), (packaging.lab, 'ROOT')]:
            patcher = mock.patch.object(module, key, self.root)
            patcher.start()
            self.addCleanup(patcher.stop)

    def test_generated_package_discloses_emulation_and_ai_assistance(self):
        with contextlib.redirect_stdout(io.StringIO()), mock.patch('sys.argv', ['assemble-package.py']):
            self.assertEqual(packaging.main(), 0)
        pkg = self.root / 'local/package/tomodachi'
        description = (pkg / 'README.txt').read_text().lower()
        desktop = (pkg / 'tomodachi-native.desktop').read_text().lower()
        self.assertIn('suyu', description)
        self.assertIn('emulation', description)
        self.assertIn('ai', description)
        self.assertIn('experimental', desktop)
        self.assertNotIn('native build', desktop)
        self.assertTrue((pkg / 'run.sh').exists())

    def test_setting_ini_twice_updates_values_without_duplicates(self):
        path = self.root / 'settings.ini'
        packaging.set_ini(path, 'Renderer', {'resolution_setup': '6'})
        packaging.set_ini(path, 'Renderer', {'resolution_setup': '7'})
        content = path.read_text()
        self.assertEqual(content.count('resolution_setup='), 1)
        self.assertIn('resolution_setup=7', content)

    def test_reassembling_package_keeps_user_save_files(self):
        with contextlib.redirect_stdout(io.StringIO()), mock.patch('sys.argv', ['assemble-package.py']):
            packaging.main()
            save = self.root / 'local/package/tomodachi/user/nand/user/save/test-save'
            save.parent.mkdir(parents=True)
            save.write_bytes(b'synthetic save; not a real game file')
            packaging.main()
        self.assertEqual(save.read_bytes(), b'synthetic save; not a real game file')


if __name__ == '__main__':
    unittest.main()
