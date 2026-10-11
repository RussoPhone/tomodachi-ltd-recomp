import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


install = load_module('linux_install', 'scripts/install.py')
assemble = load_module('assemble_package', 'scripts/assemble-package.py')


class LinuxPickerTests(unittest.TestCase):
    def test_file_picker_uses_kdialog_when_zenity_is_unavailable(self):
        expected = '/tmp/selected-game.nsp'
        with mock.patch.dict(os.environ, {'DISPLAY': ':0'}, clear=False), \
                mock.patch.object(install.shutil, 'which', side_effect=lambda name: '/usr/bin/kdialog' if name == 'kdialog' else None), \
                mock.patch.object(install.subprocess, 'run', return_value=mock.Mock(returncode=0, stdout=expected)) as dialog, \
                mock.patch('builtins.input', side_effect=AssertionError('should use the graphical picker')) as prompt:
            actual = install.pick('file', 'Choose game')

        self.assertEqual(actual, expected)
        prompt.assert_not_called()
        self.assertEqual(
            dialog.call_args.args[0],
            ['kdialog', '--title', 'Choose game', '--getopenfilename', str(Path.home()),
             '*.nsp|Switch game (*.nsp)'],
        )

    def test_directory_picker_uses_kdialog_when_zenity_is_unavailable(self):
        expected = '/tmp/selected-keys'
        with mock.patch.dict(os.environ, {'DISPLAY': ':0'}, clear=False), \
                mock.patch.object(install.shutil, 'which', side_effect=lambda name: '/usr/bin/kdialog' if name == 'kdialog' else None), \
                mock.patch.object(install.subprocess, 'run', return_value=mock.Mock(returncode=0, stdout=expected)) as dialog, \
                mock.patch('builtins.input', side_effect=AssertionError('should use the graphical picker')) as prompt:
            actual = install.pick('dir', 'Choose keys')

        self.assertEqual(actual, expected)
        prompt.assert_not_called()
        self.assertEqual(
            dialog.call_args.args[0],
            ['kdialog', '--title', 'Choose keys', '--getexistingdirectory', str(Path.home())],
        )


class LinuxLauncherTests(unittest.TestCase):
    def test_run_script_preserves_spaces_in_forwarded_arguments(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build = root / 'upstream/mk8-recomp/build/suyu-static/bin'
            installed = root / 'local/install/tomodachi/exefs'
            build.mkdir(parents=True)
            (installed / 'romfs.bin').parent.mkdir(parents=True)
            (installed / 'romfs.bin').touch()

            capture = build / 'suyu-cmd-static'
            capture.write_text(
                '#!/usr/bin/env python3\n'
                'import json, pathlib, sys\n'
                'ini = pathlib.Path("user/config/sdl2-config.ini")\n'
                'pathlib.Path("argv.json").write_text(json.dumps({"args": sys.argv[1:], "ini": ini.read_text()}))\n'
            )
            capture.chmod(0o755)

            with mock.patch.object(assemble, 'ROOT', root), \
                    mock.patch.object(assemble.lab, 'target_name', return_value='tomodachi'), \
                    mock.patch.object(assemble.lab, 'title_id', return_value='0100000000000000'), \
                    mock.patch.object(assemble.lab, 'adapter', return_value={'name': 'Tomodachi'}), \
                    mock.patch.object(sys, 'argv', ['assemble-package.py', '--name', 'tomodachi-native']):
                self.assertEqual(assemble.main(), 0)

            subprocess.run(
                [str(root / 'local/package/tomodachi/run.sh'), '--scale', '2', '--message', 'two words'],
                cwd=root / 'local/package/tomodachi', check=True,
            )
            actual = json.loads((root / 'local/package/tomodachi/argv.json').read_text())

        self.assertEqual(actual['args'], ['--message', 'two words', '-g', 'game/main'])
        self.assertIn('resolution_setup\\default=false', actual['ini'])
        self.assertIn('resolution_setup=6', actual['ini'])

    def test_run_script_updates_scale_when_the_setting_already_exists(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build = root / 'upstream/mk8-recomp/build/suyu-static/bin'
            installed = root / 'local/install/tomodachi/exefs'
            build.mkdir(parents=True)
            installed.mkdir(parents=True)
            (installed / 'romfs.bin').touch()
            (build / 'suyu-cmd-static').write_text(
                '#!/usr/bin/env python3\n'
                'import pathlib, sys\n'
                'pathlib.Path("ini.txt").write_text(pathlib.Path("user/config/sdl2-config.ini").read_text())\n'
            )
            (build / 'suyu-cmd-static').chmod(0o755)
            pkg = root / 'local/package/tomodachi'
            pkg.mkdir(parents=True)
            (pkg / 'user/config').mkdir(parents=True)
            (pkg / 'user/config/sdl2-config.ini').write_text(
                '[Renderer]\nresolution_setup\\default=true\nresolution_setup=3\n'
            )
            # The assembler replaces an existing package, so let it create the launcher and config.
            with mock.patch.object(assemble, 'ROOT', root), \
                    mock.patch.object(assemble.lab, 'target_name', return_value='tomodachi'), \
                    mock.patch.object(assemble.lab, 'title_id', return_value='0100000000000000'), \
                    mock.patch.object(assemble.lab, 'adapter', return_value={'name': 'Tomodachi'}), \
                    mock.patch.object(sys, 'argv', ['assemble-package.py']):
                self.assertEqual(assemble.main(), 0)

            subprocess.run([str(pkg / 'run.sh'), '--scale', '2'], cwd=pkg, check=True)
            actual = (pkg / 'ini.txt').read_text()

        self.assertIn('resolution_setup\\default=false', actual)
        self.assertIn('resolution_setup=6', actual)

    def test_windows_launcher_adds_missing_resolution_settings(self):
        with tempfile.TemporaryDirectory() as temp:
            pkg = Path(temp)
            assemble.write_windows_launcher(pkg, 'tomodachi-native')
            launcher = (pkg / 'run.ps1').read_text()

        self.assertIn("if ($text -notmatch '(?m)^resolution_setup=')", launcher)
        self.assertIn("if ($text -notmatch '(?m)^resolution_setup\\\\default=')", launcher)


if __name__ == '__main__':
    unittest.main()
