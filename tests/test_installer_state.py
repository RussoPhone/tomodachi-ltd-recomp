"""Installer resume/invalidation tests using only disposable synthetic files."""
import contextlib
import importlib.util
import io
from pathlib import Path
import shutil
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


class NxrtStepHelpersTests(unittest.TestCase):
    def test_boost_source_prefers_the_tree_suyu_downloaded(self):
        with tempfile.TemporaryDirectory() as tmp:
            suyu = Path(tmp)
            tree = suyu / '.cache/cpm/boost/boost-1.90.0'
            tree.mkdir(parents=True)
            with mock.patch.object(installer, 'SUYU', suyu):
                self.assertEqual(installer.boost_source(), tree)

    def test_boost_source_reuses_the_cached_download(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'cpmfile.json').write_text('{"boost": {"version": "boost-9.9.9", "artifact": "x", "repo": "a/b", "hash": "0"}}')
            tree = root / 'local/cache/boost/boost-9.9.9'
            (tree / 'libs').mkdir(parents=True)
            with mock.patch.object(installer, 'SUYU', root), mock.patch.object(installer, 'ROOT', root):
                self.assertEqual(installer.boost_source(), tree)

    def test_fmt_prefix_is_none_when_the_system_has_fmt(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'fmt').mkdir()
            with mock.patch.object(installer, 'CMAKE_DIRS', (tmp,)), mock.patch.object(installer, 'run') as run:
                self.assertIsNone(installer.fmt_prefix())
            run.assert_not_called()

    def test_fmt_prefix_builds_the_copy_suyu_downloaded(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'suyu/.cache/cpm/fmt/12.1.0'
            source.mkdir(parents=True)
            (source / 'CMakeLists.txt').write_text('')
            with mock.patch.object(installer, 'CMAKE_DIRS', ()), mock.patch.object(installer, 'ROOT', root), \
                    mock.patch.object(installer, 'SUYU', root / 'suyu'), mock.patch.object(installer, 'run') as run, \
                    mock.patch.object(installer, 'say'):
                self.assertEqual(installer.fmt_prefix(), root / 'local/deps/fmt')
            configure = [str(a) for a in run.call_args_list[0].args[0]]
            self.assertIn(str(source), configure)
            self.assertIn('-DBUILD_SHARED_LIBS=OFF', configure)
            # Built once: a second install finds it.
            (root / 'local/deps/fmt/lib/cmake/fmt').mkdir(parents=True)
            with mock.patch.object(installer, 'CMAKE_DIRS', ()), mock.patch.object(installer, 'ROOT', root), \
                    mock.patch.object(installer, 'run') as run:
                self.assertEqual(installer.fmt_prefix(), root / 'local/deps/fmt')
            run.assert_not_called()

    def test_export_with_unsplit_files_stops_before_the_compile(self):
        with tempfile.TemporaryDirectory() as tmp:
            export = Path(tmp)
            (export / 'main/src').mkdir(parents=True)
            (export / 'main/src/recompiled_main_0.c').write_bytes(b'x' * 1000)
            with mock.patch.object(installer, 'say'), mock.patch.object(installer, 'BIGGEST_UNIT_MB', 1 / 1024), \
                    mock.patch.object(installer, 'fail', side_effect=SystemExit) as fail:
                installer.check_unit_sizes(export)
                fail.assert_not_called()
                (export / 'main/src/recompiled_main_59.c').write_bytes(b'x' * 2048)
                with self.assertRaises(SystemExit):
                    installer.check_unit_sizes(export)
            self.assertIn('recompiled_main_59.c', fail.call_args.args[0])
            self.assertIn('--redo 4', fail.call_args.args[0])

    def test_steamos_hint_only_outside_a_container(self):
        osr = 'PRETTY_NAME="SteamOS"\nID=steamos\n'
        with mock.patch.object(installer.Path, 'read_text', return_value=osr), \
                mock.patch.object(installer.Path, 'exists', return_value=False):
            self.assertIn('distrobox', installer.steamos_hint())
        with mock.patch.object(installer.Path, 'read_text', return_value=osr), \
                mock.patch.object(installer.Path, 'exists', return_value=True):
            self.assertEqual(installer.steamos_hint(), '')


class FreeSpaceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        for rel in installer.CLEANABLE[:3]:
            (self.root / rel).mkdir(parents=True)
            (self.root / rel / 'f.bin').write_bytes(b'x' * 1000)
        (self.root / 'local/nxrt/bin').mkdir(parents=True)
        (self.root / 'local/nxrt/bin/nxrt').write_bytes(b'x')
        (self.root / 'local/install/tomodachi').mkdir(parents=True)
        (self.root / 'local/state').mkdir(parents=True)
        self.patches = [mock.patch.object(installer, 'ROOT', self.root),
                        mock.patch.object(installer, 'CLEANED', self.root / 'local/state/cleaned'),
                        mock.patch.object(installer, 'STATE', self.root / 'local/state')]
        for patch in self.patches:
            patch.start()
            self.addCleanup(patch.stop)

    def run_free(self, **kwargs):
        with contextlib.redirect_stdout(io.StringIO()):
            installer.free_space(**kwargs)

    def test_clean_removes_only_build_leftovers_and_keeps_the_game(self):
        self.run_free(ask=False)
        for rel in installer.CLEANABLE:
            self.assertFalse((self.root / rel).exists())
        self.assertTrue((self.root / 'local/nxrt/bin/nxrt').exists())
        self.assertTrue((self.root / 'local/install/tomodachi').is_dir())
        self.assertTrue(installer.CLEANED.exists())

    def test_clean_works_when_the_state_folder_does_not_exist_yet(self):
        shutil.rmtree(self.root / 'local/state')
        self.run_free(ask=False)
        self.assertTrue(installer.CLEANED.exists())

    def test_nothing_is_deleted_before_the_game_is_installed(self):
        (self.root / 'local/nxrt/bin/nxrt').unlink()
        self.run_free(ask=False)
        self.assertTrue((self.root / installer.CLEANABLE[0]).exists())
        self.assertFalse(installer.CLEANED.exists())

    def test_declining_keeps_everything(self):
        with mock.patch('builtins.input', return_value='n'):
            self.run_free(ask=True)
        self.assertTrue((self.root / installer.CLEANABLE[0]).exists())

    def test_after_a_clean_a_rerun_restarts_from_the_build_tools(self):
        installer.CLEANED.write_text('x')
        backend = [k for k, _, _ in installer.STEPS].index('backend') + 1
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(installer.forget_cleaned_build(len(installer.STEPS)), backend)
        self.assertFalse(installer.CLEANED.exists())
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(installer.forget_cleaned_build(3), 3)


if __name__ == '__main__':
    unittest.main()
