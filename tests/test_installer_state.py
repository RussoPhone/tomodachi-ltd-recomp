"""Installer resume/invalidation tests using only disposable synthetic files."""
import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/install.py'
spec = importlib.util.spec_from_file_location('tested_installer', SCRIPT)
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)

KEYS = [key for key, _, _ in installer.STEPS]


class InstallerStateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / 'patches').mkdir()
        (self.root / 'scripts').mkdir()
        (self.root / 'adapters/tomodachi').mkdir(parents=True)
        (self.root / 'upstream.lock.json').write_text('{"commit":"old"}')
        (self.root / 'patches/0001.patch').write_text('old patch')
        (self.root / 'scripts/compile-meter.py').write_text('old meter')
        (self.root / 'scripts/package-standalone.py').write_text('old builder')
        (self.root / 'adapters/tomodachi/target.json').write_text('{"version":1}')
        self.runs = []
        steps = [(key, key, lambda key=key: self.runs.append(key)) for key in KEYS]
        for name, value in [('ROOT', self.root), ('STATE', self.root / 'local/state'),
                            ('STEPS', steps), ('CI', False)]:
            patcher = mock.patch.object(installer, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    def run_installer(self):
        with contextlib.redirect_stdout(io.StringIO()):
            installer.main()

    def test_unmodified_inputs_do_not_repeat_steps(self):
        self.run_installer()
        self.runs.clear()
        self.run_installer()
        self.assertEqual(self.runs, [])

    def test_patch_change_invalidates_all_dependent_steps(self):
        self.run_installer()
        self.runs.clear()
        (self.root / 'patches/0001.patch').write_text('new patch')
        self.run_installer()
        self.assertEqual(self.runs, KEYS[KEYS.index('patches'):])

    def test_new_patch_file_invalidates_dependent_steps(self):
        self.run_installer()
        self.runs.clear()
        (self.root / 'patches/0002.patch').write_text('added patch')
        self.run_installer()
        self.assertEqual(self.runs, KEYS[KEYS.index('patches'):])

    def test_upstream_lock_change_invalidates_fetch_and_later(self):
        self.run_installer()
        self.runs.clear()
        (self.root / 'upstream.lock.json').write_text('{"commit":"new"}')
        self.run_installer()
        self.assertEqual(self.runs, KEYS[KEYS.index('fetch'):])

    def test_compiler_launcher_change_invalidates_package_and_later(self):
        self.run_installer()
        self.runs.clear()
        (self.root / 'scripts/compile-meter.py').write_text('new meter')
        self.run_installer()
        self.assertEqual(self.runs, KEYS[KEYS.index('package'):])

    def test_target_change_invalidates_dump_and_later(self):
        self.run_installer()
        self.runs.clear()
        (self.root / 'adapters/tomodachi/target.json').write_text('{"version":2}')
        self.run_installer()
        self.assertEqual(self.runs, KEYS[KEYS.index('dump'):])

    def test_legacy_markers_without_fingerprints_are_refreshed_safely(self):
        self.run_installer()
        for stamp in (self.root / 'local/state').glob('*.sha256'):
            stamp.unlink()
        self.runs.clear()
        self.run_installer()
        self.assertEqual(self.runs, KEYS[KEYS.index('fetch'):])

    def test_missing_intermediate_step_invalidates_later_markers(self):
        self.run_installer()
        (self.root / 'local/state/07-export.ok').unlink()
        self.runs.clear()
        self.run_installer()
        self.assertEqual(self.runs, KEYS[KEYS.index('export'):])


if __name__ == '__main__':
    unittest.main()
