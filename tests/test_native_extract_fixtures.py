import importlib.util
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


install_native = load_module('install_native_fixture', 'scripts/install-native.py')
assemble = load_module('assemble_package_fixture', 'scripts/assemble-package.py')


class NativeExtractionFixtureTests(unittest.TestCase):
    def test_rejects_romfs_paths_that_escape_the_extraction_root(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'romfs'
            root.mkdir()
            outside = Path(temp) / 'escaped.bin'
            with self.assertRaises(ValueError):
                install_native.romfs_destination(root, '/../escaped.bin', set())
            self.assertFalse(outside.exists())
            with self.assertRaises(ValueError):
                install_native.romfs_destination(root, r'..\escaped.bin', set())
            with self.assertRaises(ValueError):
                install_native.romfs_destination(root, 'C:/escaped.bin', set())

    def test_rejects_duplicate_or_portable_case_collision(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'romfs'
            root.mkdir()
            seen = set()
            install_native.romfs_destination(root, '/folder/File.bin', seen)
            with self.assertRaises(ValueError):
                install_native.romfs_destination(root, '/folder/file.bin', seen)

    def test_rejects_symlink_parent_that_escapes_the_extraction_root(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            root = base / 'romfs'
            root.mkdir()
            outside = base / 'outside'
            outside.mkdir()
            (root / 'redirect').symlink_to(outside, target_is_directory=True)
            with self.assertRaises(ValueError):
                install_native.romfs_destination(root, '/redirect/payload.bin', set())
            self.assertFalse((outside / 'payload.bin').exists())

    def test_rejects_short_reader_output_instead_of_silently_truncating(self):
        class ShortReader:
            def read(self, offset, size):
                return b'x' * (size - 1)

        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(EOFError):
                install_native.extract_file(ShortReader(), 0, 8, Path(temp) / 'file.bin')

    def test_extracts_synthetic_file_under_romfs_root(self):
        class FixtureReader:
            def read(self, offset, size):
                return b'synthetic-romfs'[:size]

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'romfs'
            root.mkdir()
            install_native.extract_romfs_file(
                FixtureReader(), 7, len(b'synthetic-romfs'), root, '/fixture/data.bin', set()
            )
            self.assertEqual((root / 'fixture/data.bin').read_bytes(), b'synthetic-romfs')

    def test_reports_destination_write_errors(self):
        class FixtureReader:
            def read(self, offset, size):
                return b'x' * size

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            parent_file = root / 'not-a-directory'
            parent_file.write_bytes(b'fixture')
            with self.assertRaises(FileExistsError):
                install_native.extract_romfs_file(
                    FixtureReader(), 0, 1, root, '/not-a-directory/file.bin', set()
                )


class PackageFallbackFixtures(unittest.TestCase):
    def test_package_fallback_copy_keeps_executable_and_runs_after_relocation(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'project'
            build = root / 'upstream/mk8-recomp/build/suyu-static/bin'
            installed = root / 'local/install/tomodachi/exefs'
            build.mkdir(parents=True)
            installed.mkdir(parents=True)
            (installed / 'romfs.bin').write_bytes(b'fixture romfs')
            executable = build / 'suyu-cmd-static'
            executable.write_text(
                '#!/usr/bin/env python3\n'
                'import json, pathlib, sys\n'
                'pathlib.Path("argv.json").write_text(json.dumps(sys.argv[1:]))\n'
            )
            executable.chmod(0o755)
            with mock.patch.object(assemble, 'ROOT', root), \
                    mock.patch.object(assemble.lab, 'target_name', return_value='tomodachi'), \
                    mock.patch.object(assemble.lab, 'title_id', return_value='0100000000000000'), \
                    mock.patch.object(assemble.lab, 'adapter', return_value={'name': 'Tomodachi'}), \
                    mock.patch.object(assemble.os, 'link', side_effect=OSError('hardlinks unavailable')), \
                    mock.patch('sys.argv', ['assemble-package.py', '--name', 'tomodachi-native']):
                self.assertEqual(assemble.main(), 0)

            package = root / 'local/package/tomodachi'
            packaged_exe = package / 'tomodachi-native'
            self.assertTrue(packaged_exe.stat().st_mode & 0o111)
            self.assertEqual(
                hashlib.sha256(packaged_exe.read_bytes()).digest(),
                hashlib.sha256(executable.read_bytes()).digest(),
            )
            self.assertEqual(
                hashlib.sha256((package / 'game/romfs.bin').read_bytes()).digest(),
                hashlib.sha256((installed / 'romfs.bin').read_bytes()).digest(),
            )
            moved_package = Path(temp) / 'moved package'
            package.rename(moved_package)
            os.chmod(moved_package / 'run.sh', 0o755)
            subprocess.run([str(moved_package / 'run.sh'), '--message', 'fixture words'],
                           cwd=moved_package, check=True)
            self.assertEqual(
                json.loads((moved_package / 'argv.json').read_text()),
                ['--message', 'fixture words', '-g', 'game/main'],
            )


if __name__ == '__main__':
    unittest.main()
